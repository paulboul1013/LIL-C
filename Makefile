CPPFLAGS += -Iinclude
CFLAGS += -Wall -Wextra -Wpedantic

# The compiler binary built by 'make'.
COMPILER = lilc

CORE_SRCS = src/token.c src/source.c src/diagnostic.c src/lexer.c \
	src/ast.c src/operator.c src/parser.c src/ast_print.c src/symbol.c \
	src/sema.c src/lil.c src/ir.c src/ir_lower.c src/ir_parse.c \
	src/ir_verify.c src/ir_interp.c src/ir_opt.c
# Debug printers used only by the CLI (and the fuzz harness).
DEBUG_SRCS = src/ast_debug.c src/token_debug.c
MAIN_SRCS = src/main.c $(CORE_SRCS) $(DEBUG_SRCS)
TEST_SRCS = tests/test_lil.c $(CORE_SRCS)
# Semantic analysis (stage 3) unit tests.
SEMA_TEST_SRCS = tests/test_sema.c $(CORE_SRCS)
# IR (stage 4) unit tests.
IR_TEST_SRCS = tests/test_ir.c $(CORE_SRCS)
# IR optimizer (stage 4 part B) unit tests.
IR_OPT_TEST_SRCS = tests/test_ir_opt.c $(CORE_SRCS)
HEADERS = $(wildcard include/*.h)
TEST_HEADERS = tests/test_framework.h
# A TEST() that is never passed to RUN_TEST() would silently not run;
# make that a build error instead of a warning.
TEST_CFLAGS = -Werror=unused-function

UNIT_TEST = tests/test_lil
SEMA_TEST = tests/test_sema
IR_TEST = tests/test_ir
IR_OPT_TEST = tests/test_ir_opt
FRAMEWORK_SELFTEST = tests/test_framework_selftest
GOLDEN_RUNNER = tests/test_cli.sh
EXAMPLES = $(wildcard examples/*.lil)
GOLDEN_FIXTURES = $(shell find tests/golden -type f 2>/dev/null)

BUILD_DIR = build

DIFFTEST_COUNT ?= 300
DIFFTEST_QUICK_COUNT ?= 60
DIFFTEST_SEED ?= 20261009

# --- sanitizers --------------------------------------------------------------
SANITIZE_DIR = $(BUILD_DIR)/sanitize
SANITIZER_FLAGS = -fsanitize=address,undefined -fno-sanitize-recover=all \
	-fno-omit-frame-pointer -g
SANITIZER_ENV = ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
SANITIZED_COMPILER = $(SANITIZE_DIR)/$(COMPILER)
SANITIZED_TEST = $(SANITIZE_DIR)/test_lil
SANITIZED_SEMA_TEST = $(SANITIZE_DIR)/test_sema
SANITIZED_IR_TEST = $(SANITIZE_DIR)/test_ir
SANITIZED_IR_OPT_TEST = $(SANITIZE_DIR)/test_ir_opt

# --- coverage ----------------------------------------------------------------
COVERAGE_DIR = $(BUILD_DIR)/coverage
COVERAGE_FLAGS = --coverage -O0 -g
# Fail 'make coverage' when total line coverage is below this percentage
# (the plan's per-stage gate).
COVERAGE_MIN ?= 90
# Each stage's own files must also reach COVERAGE_FILE_MIN on their own,
# so a well-covered file cannot hide a poorly tested stage. Add new stage
# files here (for example src/ir.c, src/backend.c) as they appear.
COVERAGE_FILE_MIN ?= 90
COVERAGE_GATED_SRCS = src/lexer.c src/parser.c src/sema.c src/symbol.c \
	src/ir.c src/ir_lower.c src/ir_parse.c src/ir_verify.c src/ir_interp.c \
	src/ir_opt.c
COVERAGE_REPORT_SRCS = $(sort $(MAIN_SRCS))
COVERAGE_ALL_SRCS = $(sort $(MAIN_SRCS) tests/test_lil.c tests/test_sema.c \
	tests/test_ir.c tests/test_ir_opt.c)
COVERAGE_OBJS = $(patsubst %.c,$(COVERAGE_DIR)/obj/%.o,$(COVERAGE_ALL_SRCS))
COVERAGE_CORE_OBJS = $(patsubst %.c,$(COVERAGE_DIR)/obj/%.o,$(CORE_SRCS))

# --- fuzzing (clang + libFuzzer) ---------------------------------------------
FUZZ_CC ?= clang
FUZZ_DIR = $(BUILD_DIR)/fuzz
FUZZ_FLAGS = -g -O1 -fsanitize=fuzzer,address,undefined \
	-fno-sanitize-recover=all
FUZZ_FRONTEND = $(FUZZ_DIR)/fuzz_frontend
# The IR text path (--ir-input): parser, verifier, dump, interpreter.
FUZZ_IR = $(FUZZ_DIR)/fuzz_ir
# Targets 'make fuzz-run' runs, one after the other, each for FUZZ_TIME
# seconds; pick one with FUZZ_TARGET=frontend or FUZZ_TARGET=ir.
FUZZ_TARGET ?= frontend ir
FUZZ_TIME ?= 60
# Largest generated input. Raise it (for example FUZZ_MAX_LEN=1048576) to
# explore very large inputs; deep-nesting seeds near the §6.3 limits are
# added to the seed corpus either way.
FUZZ_MAX_LEN ?= 4096
# The corpora live outside $(BUILD_DIR) so 'make clean' keeps them and long
# fuzzing sessions can be resumed across several bounded runs
# (for example 'make fuzz-run FUZZ_TIME=600' repeated). They are
# git-ignored.
FUZZ_CORPUS ?= fuzz/corpus
FUZZ_IR_CORPUS ?= fuzz/corpus-ir
FUZZ_SEEDS = $(FUZZ_DIR)/seeds
FUZZ_IR_SEEDS = $(FUZZ_DIR)/seeds-ir

all: $(COMPILER)

$(COMPILER): $(MAIN_SRCS) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(MAIN_SRCS)

$(UNIT_TEST): $(TEST_SRCS) $(HEADERS) $(TEST_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_SRCS)

$(SEMA_TEST): $(SEMA_TEST_SRCS) $(HEADERS) $(TEST_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) -o $@ $(SEMA_TEST_SRCS)

$(IR_TEST): $(IR_TEST_SRCS) $(HEADERS) $(TEST_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) -o $@ $(IR_TEST_SRCS)

$(IR_OPT_TEST): $(IR_OPT_TEST_SRCS) $(HEADERS) $(TEST_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) -o $@ $(IR_OPT_TEST_SRCS)

$(FRAMEWORK_SELFTEST): tests/test_framework_selftest.c $(TEST_HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) -o $@ \
		tests/test_framework_selftest.c

test: $(COMPILER) $(UNIT_TEST) $(SEMA_TEST) $(IR_TEST) $(IR_OPT_TEST) \
		$(FRAMEWORK_SELFTEST) $(GOLDEN_RUNNER) $(GOLDEN_FIXTURES) $(EXAMPLES)
	sh tests/check_framework.sh ./$(FRAMEWORK_SELFTEST)
	sh tests/check_golden_runner.sh
	./$(UNIT_TEST)
	./$(SEMA_TEST)
	./$(IR_TEST)
	./$(IR_OPT_TEST)
	sh $(GOLDEN_RUNNER) ./$(COMPILER)
	sh $(GOLDEN_RUNNER) --interp-flag -O1 ./$(COMPILER) tests/golden/interp
	sh tests/check_examples.sh ./$(COMPILER)
	$(MAKE) --no-print-directory difftest DIFFTEST_COUNT=$(DIFFTEST_QUICK_COUNT)

# Differential test (plan "差分測試", spec §5.7): DIFFTEST_COUNT random
# programs from tests/tools/gen_lil.py (fixed seed) must give the same
# stdout and status on the independent oracle tests/tools/lil_oracle.py,
# --interp -O0 and --interp -O1; the -O0 and -O1 dumps must be fixpoints
# of --ir-input (round trip, -O1 idempotence) and -O1 never has more IR
# instructions. 'make test' runs a smaller DIFFTEST_QUICK_COUNT. Needs
# python3; skipped with a note when it is missing.
difftest: $(COMPILER)
	@if command -v python3 >/dev/null 2>&1; then \
		python3 tests/tools/diff_test.py ./$(COMPILER) \
			--count $(DIFFTEST_COUNT) --seed $(DIFFTEST_SEED); \
	else \
		echo "difftest: python3 not found, skipped"; \
	fi

# Regenerate every golden expectation from the current compiler.
# Review 'git diff tests/golden' before committing the result.
update-golden: $(COMPILER)
	sh $(GOLDEN_RUNNER) --update ./$(COMPILER)

$(SANITIZED_COMPILER): $(MAIN_SRCS) $(HEADERS)
	@mkdir -p $(SANITIZE_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_FLAGS) -o $@ $(MAIN_SRCS)

$(SANITIZED_TEST): $(TEST_SRCS) $(HEADERS) $(TEST_HEADERS)
	@mkdir -p $(SANITIZE_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(SANITIZER_FLAGS) \
		-o $@ $(TEST_SRCS)

$(SANITIZED_SEMA_TEST): $(SEMA_TEST_SRCS) $(HEADERS) $(TEST_HEADERS)
	@mkdir -p $(SANITIZE_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(SANITIZER_FLAGS) \
		-o $@ $(SEMA_TEST_SRCS)

$(SANITIZED_IR_TEST): $(IR_TEST_SRCS) $(HEADERS) $(TEST_HEADERS)
	@mkdir -p $(SANITIZE_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(SANITIZER_FLAGS) \
		-o $@ $(IR_TEST_SRCS)

$(SANITIZED_IR_OPT_TEST): $(IR_OPT_TEST_SRCS) $(HEADERS) $(TEST_HEADERS)
	@mkdir -p $(SANITIZE_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) $(SANITIZER_FLAGS) \
		-o $@ $(IR_OPT_TEST_SRCS)

sanitize: $(SANITIZED_COMPILER) $(SANITIZED_TEST) $(SANITIZED_SEMA_TEST) \
		$(SANITIZED_IR_TEST) $(SANITIZED_IR_OPT_TEST) $(GOLDEN_RUNNER) \
		$(GOLDEN_FIXTURES)
	$(SANITIZER_ENV) ./$(SANITIZED_TEST)
	$(SANITIZER_ENV) ./$(SANITIZED_SEMA_TEST)
	$(SANITIZER_ENV) ./$(SANITIZED_IR_TEST)
	$(SANITIZER_ENV) ./$(SANITIZED_IR_OPT_TEST)
	$(SANITIZER_ENV) sh $(GOLDEN_RUNNER) ./$(SANITIZED_COMPILER)
	$(SANITIZER_ENV) sh $(GOLDEN_RUNNER) --interp-flag -O1 \
		./$(SANITIZED_COMPILER) tests/golden/interp
	$(SANITIZER_ENV) sh tests/check_examples.sh ./$(SANITIZED_COMPILER)

$(COVERAGE_DIR)/obj/tests/test_lil.o: CFLAGS += $(TEST_CFLAGS)
$(COVERAGE_DIR)/obj/tests/test_sema.o: CFLAGS += $(TEST_CFLAGS)
$(COVERAGE_DIR)/obj/tests/test_ir.o: CFLAGS += $(TEST_CFLAGS)
$(COVERAGE_DIR)/obj/tests/test_ir_opt.o: CFLAGS += $(TEST_CFLAGS)

$(COVERAGE_DIR)/obj/%.o: %.c $(HEADERS) $(TEST_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COVERAGE_FLAGS) -c -o $@ $<

$(COVERAGE_DIR)/$(COMPILER): $(COVERAGE_OBJS)
	$(CC) $(COVERAGE_FLAGS) -o $@ \
		$(patsubst %.c,$(COVERAGE_DIR)/obj/%.o,$(MAIN_SRCS))

$(COVERAGE_DIR)/test_lil: $(COVERAGE_OBJS)
	$(CC) $(COVERAGE_FLAGS) -o $@ \
		$(COVERAGE_DIR)/obj/tests/test_lil.o $(COVERAGE_CORE_OBJS)

$(COVERAGE_DIR)/test_sema: $(COVERAGE_OBJS)
	$(CC) $(COVERAGE_FLAGS) -o $@ \
		$(COVERAGE_DIR)/obj/tests/test_sema.o $(COVERAGE_CORE_OBJS)

$(COVERAGE_DIR)/test_ir: $(COVERAGE_OBJS)
	$(CC) $(COVERAGE_FLAGS) -o $@ \
		$(COVERAGE_DIR)/obj/tests/test_ir.o $(COVERAGE_CORE_OBJS)

$(COVERAGE_DIR)/test_ir_opt: $(COVERAGE_OBJS)
	$(CC) $(COVERAGE_FLAGS) -o $@ \
		$(COVERAGE_DIR)/obj/tests/test_ir_opt.o $(COVERAGE_CORE_OBJS)

coverage: $(COVERAGE_DIR)/$(COMPILER) $(COVERAGE_DIR)/test_lil \
		$(COVERAGE_DIR)/test_sema $(COVERAGE_DIR)/test_ir \
		$(COVERAGE_DIR)/test_ir_opt
	find $(COVERAGE_DIR) -name '*.gcda' -delete
	./$(COVERAGE_DIR)/test_lil >/dev/null
	./$(COVERAGE_DIR)/test_sema >/dev/null
	./$(COVERAGE_DIR)/test_ir >/dev/null
	./$(COVERAGE_DIR)/test_ir_opt >/dev/null
	sh $(GOLDEN_RUNNER) ./$(COVERAGE_DIR)/$(COMPILER)
	sh tests/check_examples.sh ./$(COVERAGE_DIR)/$(COMPILER) >/dev/null
	sh tests/coverage.sh -f $(COVERAGE_FILE_MIN) \
		$(addprefix -g ,$(COVERAGE_GATED_SRCS)) \
		$(COVERAGE_DIR)/obj $(COVERAGE_MIN) $(COVERAGE_REPORT_SRCS)

$(FUZZ_FRONTEND): fuzz/fuzz_frontend.c fuzz/fuzz_opt.h $(CORE_SRCS) $(DEBUG_SRCS) $(HEADERS)
	@mkdir -p $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(CFLAGS) $(FUZZ_FLAGS) -o $@ \
		fuzz/fuzz_frontend.c $(CORE_SRCS) $(DEBUG_SRCS)

$(FUZZ_IR): fuzz/fuzz_ir.c fuzz/fuzz_opt.h $(CORE_SRCS) $(HEADERS)
	@mkdir -p $(FUZZ_DIR)
	$(FUZZ_CC) $(CPPFLAGS) $(CFLAGS) $(FUZZ_FLAGS) -o $@ \
		fuzz/fuzz_ir.c $(CORE_SRCS)

fuzz: $(FUZZ_FRONTEND) $(FUZZ_IR)

# Runs every target in FUZZ_TARGET for FUZZ_TIME seconds each.
fuzz-run: $(addprefix fuzz-run-,$(FUZZ_TARGET))

# Seeds come from examples/ and the golden fixtures; new inputs that
# libFuzzer finds are kept in $(FUZZ_CORPUS). Crashes are written to
# $(FUZZ_DIR)/crash-*.
fuzz-run-frontend: $(FUZZ_FRONTEND)
	@mkdir -p $(FUZZ_CORPUS) $(FUZZ_SEEDS)
	cp examples/*.lil examples/accept/*.lil $(FUZZ_SEEDS)/
	find tests/golden -name '*.lil' -exec cp {} $(FUZZ_SEEDS)/ \;
	awk 'BEGIN { for (i = 0; i < 300; i++) printf "a = "; print "1;" }' \
		>$(FUZZ_SEEDS)/deep_assignment.lil
	awk 'BEGIN { printf "1"; for (i = 0; i < 1100; i++) printf "+1"; print ";" }' \
		>$(FUZZ_SEEDS)/long_chain.lil
	$(SANITIZER_ENV) ./$(FUZZ_FRONTEND) \
		-max_total_time=$(FUZZ_TIME) -timeout=10 -rss_limit_mb=2048 \
		-max_len=$(FUZZ_MAX_LEN) \
		-dict=fuzz/lil.dict -artifact_prefix=$(FUZZ_DIR)/ \
		$(FUZZ_CORPUS) $(FUZZ_SEEDS)

# Seeds: every hand-written .ir fixture, plus the --dump-ir of every
# example and golden .lil program that compiles. New inputs go to
# $(FUZZ_IR_CORPUS); crashes to $(FUZZ_DIR)/ir-crash-*.
fuzz-run-ir: $(FUZZ_IR) $(COMPILER)
	@mkdir -p $(FUZZ_IR_CORPUS) $(FUZZ_IR_SEEDS)
	find tests/golden -name '*.ir' -exec cp {} $(FUZZ_IR_SEEDS)/ \;
	for f in examples/*.lil examples/accept/*.lil \
			$$(find tests/golden -name '*.lil'); do \
		./$(COMPILER) "$$f" --dump-ir \
			>$(FUZZ_IR_SEEDS)/dump_$$(echo "$$f" | tr '/' '_').ir \
			2>/dev/null || true; \
	done
	find $(FUZZ_IR_SEEDS) -name 'dump_*' -size 0 -delete
	$(SANITIZER_ENV) ./$(FUZZ_IR) \
		-max_total_time=$(FUZZ_TIME) -timeout=10 -rss_limit_mb=2048 \
		-max_len=$(FUZZ_MAX_LEN) \
		-dict=fuzz/ir.dict -artifact_prefix=$(FUZZ_DIR)/ir- \
		$(FUZZ_IR_CORPUS) $(FUZZ_IR_SEEDS)

clean:
	rm -f $(COMPILER) main $(UNIT_TEST) $(SEMA_TEST) $(IR_TEST) $(IR_OPT_TEST) \
		$(FRAMEWORK_SELFTEST) \
		*.gcov
	rm -rf $(BUILD_DIR)

.PHONY: all test difftest update-golden sanitize coverage fuzz fuzz-run \
	fuzz-run-frontend fuzz-run-ir clean
