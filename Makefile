CPPFLAGS += -Iinclude
CFLAGS += -Wall -Wextra -Wpedantic

CORE_SRCS = src/token.c src/source.c src/diagnostic.c src/lexer.c \
	src/ast.c src/parser.c src/lil.c
MAIN_SRCS = src/main.c $(CORE_SRCS) src/ast_debug.c
HEADERS = $(wildcard include/*.h)

main: $(MAIN_SRCS) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(MAIN_SRCS)

clean:
	rm -f main

.PHONY: clean
