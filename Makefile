CFLAGS= -Wall -Wextra -Wpedantic -Iinclude
main:
	gcc $(CFLAGS) -o main src/main.c src/token.c src/lexer.c

clean:
	rm -rf main