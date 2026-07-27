# unojs host test build. Plain gcc, no OS, no web code - if this ever needs
# something from pc64/ or unoweb/, the separation has been broken.
CC      ?= gcc
CFLAGS  ?= -std=c99 -Wall -Wextra -O1 -g
SRC      = ../ujs_core.c ../ujs_lex.c ../ujs_comp.c ../ujs_vm.c ../ujs_lib.c ../ujs_api.c
LDLIBS   = -lm

run_tests: run_tests.c $(SRC) ../unojs.h ../ujs_int.h ../ujs_lex.h
	$(CC) $(CFLAGS) -o $@ run_tests.c $(SRC) $(LDLIBS)

.PHONY: test valgrind clean
test: run_tests
	./run_tests

# Memory discipline is a merge gate for this subsystem: the collector is
# non-moving and hand-written, so a leak or an invalid read is a real defect.
valgrind: run_tests
	valgrind --error-exitcode=9 --leak-check=full --errors-for-leak-kinds=definite ./run_tests

clean:
	rm -f run_tests
