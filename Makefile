CPPFLAGS += -Iinclude
CFLAGS += -Wall -Wextra -Wpedantic

CORE_SRCS = src/token.c src/source.c src/diagnostic.c src/lexer.c \
	src/ast.c src/parser.c src/lil.c
MAIN_SRCS = src/main.c $(CORE_SRCS) src/ast_debug.c
TEST_SRCS = tests/test_lil.c $(CORE_SRCS)
SANITIZED_TEST = tests/test_lil_sanitized
SANITIZER_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer
HEADERS = $(wildcard include/*.h)
CLI_TEST_SCRIPT = tests/test_cli.sh
CLI_FIXTURES = $(wildcard tests/fixtures/cli/*)

main: $(MAIN_SRCS) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(MAIN_SRCS)

tests/test_lil: $(TEST_SRCS) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(TEST_SRCS)

$(SANITIZED_TEST): $(TEST_SRCS) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZER_FLAGS) -o $@ $(TEST_SRCS)

test: main tests/test_lil $(CLI_TEST_SCRIPT) $(CLI_FIXTURES)
	./tests/test_lil
	sh $(CLI_TEST_SCRIPT) ./main

sanitize: $(SANITIZED_TEST)
	$(SANITIZER_ENV) ./$(SANITIZED_TEST)

clean:
	rm -f main tests/test_lil $(SANITIZED_TEST)

.PHONY: test sanitize clean
