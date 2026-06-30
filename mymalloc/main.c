#include <mymalloc.h>

#include <stdint.h>
#include <stdio.h>

static int fail(const char *message) {
    fprintf(stderr, "mymalloc main-test failed: %s\n", message);
    return 1;
}

static int expect(int condition, const char *message) {
    if (!condition) {
        return fail(message);
    }
    return 0;
}

static int is_aligned(const void *ptr) {
    return ((uintptr_t)ptr % ALIGNMENT) == 0;
}

static int test_zero_size_returns_null(void) {
    return expect(mymalloc(0) == NULL, "mymalloc(0) should return NULL");
}

static int test_alignment_and_payload(void) {
    unsigned char *ptr = mymalloc(13);
    if (expect(ptr != NULL, "mymalloc(13) should succeed") != 0) {
        return 1;
    }
    if (expect(is_aligned(ptr), "allocated pointer should satisfy ALIGNMENT") !=
        0) {
        myfree(ptr);
        return 1;
    }

    for (size_t i = 0; i < 13; i++) {
        ptr[i] = (unsigned char)(0xA0u + i);
    }
    for (size_t i = 0; i < 13; i++) {
        if (expect(ptr[i] == (unsigned char)(0xA0u + i),
                   "payload bytes should be readable after writes") != 0) {
            myfree(ptr);
            return 1;
        }
    }

    myfree(ptr);
    return 0;
}

static int test_reuse_after_free(void) {
    void *first = mymalloc(32);
    if (expect(first != NULL, "first mymalloc(32) should succeed") != 0) {
        return 1;
    }

    myfree(first);

    void *second = mymalloc(32);
    if (expect(second != NULL, "second mymalloc(32) should succeed") != 0) {
        return 1;
    }
    if (expect(second == first, "same-size allocation should reuse freed block") !=
        0) {
        myfree(second);
        return 1;
    }

    myfree(second);
    return 0;
}

int main(void) {
    if (test_zero_size_returns_null() != 0) {
        return 1;
    }
    if (test_alignment_and_payload() != 0) {
        return 1;
    }
    if (test_reuse_after_free() != 0) {
        return 1;
    }

    puts("mymalloc main-test passed");
    return 0;
}
