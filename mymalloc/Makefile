NAME := $(shell basename $(PWD))
export MODULE := M5

SRCS   := $(shell find tests/ -maxdepth 1 -name "*.c")
CFLAGS := -I.

all: $(NAME) check

.PHONY: local-build local-test local-test-wrap local-test-perf local-check

LOCAL_CC     := gcc
LOCAL_COMMON := -O2 -std=gnu2x -ggdb -Wall -I. -I../testkit
LOCAL_SRCS   := start.c mymalloc.c tests/main.c tests/tests-trivial.c ../testkit/testkit.c

local-build:
	$(LOCAL_CC) $(LOCAL_COMMON) \
	    $(LOCAL_SRCS) -pthread -o /tmp/mymalloc-local-tests-plain

local-test: local-build
	TK_VERBOSE=1 /tmp/mymalloc-local-tests-plain

local-test-wrap:
	$(LOCAL_CC) $(LOCAL_COMMON) -DLOCAL_VMALLOC_WRAP \
	    $(LOCAL_SRCS) -pthread -Wl,--wrap=vmalloc -Wl,--wrap=vmfree \
	    -o /tmp/mymalloc-local-tests
	TK_VERBOSE=1 /tmp/mymalloc-local-tests

local-test-perf:
	$(LOCAL_CC) $(LOCAL_COMMON) -DLOCAL_VMALLOC_WRAP -DLOCAL_PERF_SMOKE \
	    $(LOCAL_SRCS) -pthread -Wl,--wrap=vmalloc -Wl,--wrap=vmfree \
	    -o /tmp/mymalloc-local-tests-perf
	TK_VERBOSE=1 /tmp/mymalloc-local-tests-perf

local-check:
	$(LOCAL_CC) -DFREESTANDING -I. -ffreestanding -nostdlib -nostartfiles \
	    -Wall -Werror -fsyntax-only *.c

check:
	gcc -DFREESTANDING -I. \
	    -ffreestanding -static -nostdlib -nostartfiles -no-pie \
	    -o malloc-check *.c && \
	        rm -f malloc-check

include ../oslab.mk
include ../.shadow/oslab.mk
