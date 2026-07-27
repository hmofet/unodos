# unoweb host test build. Plain gcc - and deliberately NO unojs. The web core
# must render a document with no JavaScript engine linked at all; if this
# Makefile ever needs unojs/, the M2 separation has been broken.
CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O1 -g
SRC      = ../uw_dom.c ../uw_html.c ../uw_css.c ../uw_style.c

run_tests: run_tests.c $(SRC) ../unoweb.h ../uw_int.h
	$(CC) $(CFLAGS) -o $@ run_tests.c $(SRC)

.PHONY: test asan clean
test: run_tests
	./run_tests

# The parser eats untrusted bytes and the DOM is arena-allocated by hand, so
# sanitizers are a merge gate for this subsystem, not an optional extra.
asan: run_tests.c $(SRC)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o run_tests_asan run_tests.c $(SRC)
	ASAN_OPTIONS=detect_leaks=1 ./run_tests_asan

clean:
	rm -f run_tests run_tests_asan
