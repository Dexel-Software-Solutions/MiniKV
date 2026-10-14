CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g
# handlers share one signature, so unused params are expected
WARN    := -Wall -Wextra -Wpedantic -Werror -Wno-unused-parameter
SAN     := -fsanitize=address,undefined -fno-omit-frame-pointer
SRC     := src/server.c src/dict.c src/resp.c

.PHONY: all asan test unit itest fuzz clean
all: minikv

minikv: $(SRC) src/dict.h src/resp.h
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRC)

minikv-asan: $(SRC) src/dict.h src/resp.h
	$(CC) -std=c11 -O1 -g $(SAN) $(WARN) -o $@ $(SRC)

tests/test_unit: tests/test_unit.c src/dict.c src/resp.c
	$(CC) -std=c11 -g $(SAN) $(WARN) -Isrc -o $@ $^

unit: tests/test_unit
	./tests/test_unit

itest: minikv-asan
	MINIKV=./minikv-asan python3 tests/integration.py

test: unit itest

# needs clang: AFL++/libFuzzer both work with this harness
fuzz: fuzz/fuzz_resp.c src/resp.c
	clang -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -Isrc -o fuzz/fuzz_resp $^

clean:
	rm -f minikv minikv-asan tests/test_unit fuzz/fuzz_resp *.aof
