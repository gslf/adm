CFLAGS = -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE
ifneq ($(OS),Windows_NT)
CFLAGS += -pthread
endif
ifeq ($(shell uname -s),Linux)
LDLIBS += -ldl
endif

SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)

adm: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	$(RM) adm

.PHONY: clean test bench-search
test:
	CC="$(CC)" CFLAGS="$(CFLAGS)" LDLIBS="$(LDLIBS)" python3 tests/run_controls.py

bench-search:
	CC="$(CC)" CFLAGS="$(CFLAGS)" python3 tests/bench_search.py
