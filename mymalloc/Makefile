NAME := $(shell basename $(PWD))
export MODULE := M5

CFLAGS := -I.
BUILD_DIR := build
LOCAL_TEST_BIN := $(BUILD_DIR)/mymalloc-tests
MAIN_TEST_BIN := $(BUILD_DIR)/mymalloc-main-test

all: $(NAME) check

check:
	gcc -DFREESTANDING -I. \
	    -ffreestanding -static -nostdlib -nostartfiles -no-pie \
	    -c mymalloc.c start.c && \
	        rm -f *.o

.PHONY: test main-test clean
test: $(LOCAL_TEST_BIN)
	env TK_VERBOSE=1 $(LOCAL_TEST_BIN)

main-test: $(MAIN_TEST_BIN)
	$(MAIN_TEST_BIN)

clean:
	rm -rf $(BUILD_DIR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(LOCAL_TEST_BIN): mymalloc.c start.c tests/tests-trivial.c tests/main.c \
		mymalloc.h ../testkit/testkit.c ../testkit/testkit.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -I. -I../testkit -O2 -std=gnu2x -ggdb -Wall \
		./mymalloc.c ./start.c ./tests/tests-trivial.c \
		../testkit/testkit.c ./tests/main.c \
		-o $@ -pthread

$(MAIN_TEST_BIN): mymalloc.c start.c main.c mymalloc.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -I. -O2 -std=gnu2x -ggdb -Wall \
		./mymalloc.c ./start.c ./main.c \
		-o $@
