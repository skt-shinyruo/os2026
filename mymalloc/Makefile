NAME := $(shell basename $(PWD))
export MODULE := M5

SRCS   := $(shell find tests/ -maxdepth 1 -name "*.c")
CFLAGS := -I.

all: $(NAME) check

test-local:
	$(CC) $(CFLAGS) ./mymalloc.c ./start.c tests/*.c ../testkit/testkit.c \
	    -pthread -o malloc-local-test
	TK_RUN=1 ./malloc-local-test

check:
	gcc -DFREESTANDING -I. \
	    -ffreestanding -static -nostdlib -nostartfiles -no-pie \
	    -o malloc-check *.c && \
	        rm -f malloc-check

include ../oslab.mk
include ../.shadow/oslab.mk
