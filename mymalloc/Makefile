NAME := $(shell basename $(PWD))
export MODULE := M5

SRCS   := $(shell find tests/ -maxdepth 1 -name "*.c")
CFLAGS := -I.
LOCAL_TEST_BIN := /tmp/mymalloc-tests

all: $(NAME) check

check:
	gcc -DFREESTANDING -I. \
	    -ffreestanding -static -nostdlib -nostartfiles -no-pie \
	    -o malloc-check *.c && \
	        rm -f malloc-check

.PHONY: test
test: $(LOCAL_TEST_BIN)
	env TK_VERBOSE=1 $(LOCAL_TEST_BIN)

$(LOCAL_TEST_BIN): mymalloc.c start.c tests/tests-trivial.c tests/main.c \
		mymalloc.h ../testkit/testkit.c ../testkit/testkit.h
	$(CC) -I. -I../testkit -O2 -std=gnu2x -ggdb -Wall \
		./mymalloc.c ./start.c ./tests/tests-trivial.c \
		../testkit/testkit.c ./tests/main.c \
		-o $@ -pthread
