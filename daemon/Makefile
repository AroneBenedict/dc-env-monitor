CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -D_GNU_SOURCE
LDLIBS = -lm
SRC = src/common.c src/config.c src/sample.c src/alarm.c src/message.c src/spool.c src/net.c src/log.c src/engine.c
OBJ = $(SRC:src/%.c=build/%.o)

.PHONY: all test asan valgrind fuzz fuzz-lite clean

all: build/envmond

build/%.o: src/%.c src/*.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/envmond: $(OBJ) build/main.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/test_envmond: tests/test_envmond.c $(OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ $^ $(LDLIBS)

test: build/test_envmond
	./build/test_envmond

# AddressSanitizer + UndefinedBehaviorSanitizer build of the tests and the daemon.
asan:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(CFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test all

valgrind: build/test_envmond
	valgrind --error-exitcode=1 --leak-check=full -q ./build/test_envmond

# libFuzzer harness for the input-line and config parsers (needs clang).
FUZZ_TIME ?= 30
fuzz: | build
	clang -std=c11 -g -O1 -D_GNU_SOURCE -fsanitize=fuzzer,address,undefined -Isrc fuzz/fuzz_input.c \
		src/common.c src/config.c src/sample.c src/alarm.c src/message.c src/engine.c -lm -o build/fuzz_input
	./build/fuzz_input -max_total_time=$(FUZZ_TIME) -max_len=4096 fuzz/corpus

build:
	mkdir -p build

clean:
	rm -rf build

# Same harness without libFuzzer: random and mutated inputs under ASan + UBSan (gcc or clang).
FUZZ_ITERATIONS ?= 1000000
fuzz-lite: | build
	$(CC) -std=c11 -g -O1 -D_GNU_SOURCE -fsanitize=address,undefined -fno-sanitize-recover=all -Isrc \
		fuzz/random_driver.c fuzz/fuzz_input.c src/common.c src/config.c src/sample.c src/alarm.c \
		src/message.c src/engine.c -lm -o build/fuzz_lite
	./build/fuzz_lite $(FUZZ_ITERATIONS)
