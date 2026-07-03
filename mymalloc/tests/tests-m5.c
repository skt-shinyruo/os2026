#include <mymalloc.h>
#include <pthread.h>
#include <stdint.h>
#include <testkit.h>

static void fill_bytes(void *ptr, size_t size, unsigned char value) {
    unsigned char *p = ptr;
    for (size_t i = 0; i < size; i++) {
        p[i] = value;
    }
}

static void check_bytes(void *ptr, size_t size, unsigned char value) {
    unsigned char *p = ptr;
    for (size_t i = 0; i < size; i++) {
        tk_assert(p[i] == value, "payload byte changed at %zu", i);
    }
}

UnitTest(m5_alignment_and_distinct_blocks) {
    enum { N = 256 };
    void *ptrs[N];
    size_t sizes[N];

    for (int i = 0; i < N; i++) {
        sizes[i] = (size_t)((i * 17) % 1024) + 1;
        ptrs[i] = mymalloc(sizes[i]);
        tk_assert(ptrs[i] != NULL, "mymalloc(%zu) returned NULL", sizes[i]);
        tk_assert(((uintptr_t)ptrs[i] & 7) == 0,
                  "mymalloc returned unaligned pointer %p", ptrs[i]);
        fill_bytes(ptrs[i], sizes[i], (unsigned char)i);
    }

    for (int i = 0; i < N; i++) {
        check_bytes(ptrs[i], sizes[i], (unsigned char)i);
        for (int j = i + 1; j < N; j++) {
            uintptr_t a0 = (uintptr_t)ptrs[i];
            uintptr_t a1 = a0 + sizes[i];
            uintptr_t b0 = (uintptr_t)ptrs[j];
            uintptr_t b1 = b0 + sizes[j];
            tk_assert(a1 <= b0 || b1 <= a0,
                      "blocks %d and %d overlap: [%p,%p) [%p,%p)",
                      i, j, (void *)a0, (void *)a1, (void *)b0, (void *)b1);
        }
    }

    for (int i = 0; i < N; i++) {
        myfree(ptrs[i]);
    }
}

UnitTest(m5_reuses_freed_block) {
    void *p = mymalloc(128);
    tk_assert(p != NULL, "first allocation failed");
    fill_bytes(p, 128, 0xa5);
    myfree(p);

    void *q = mymalloc(128);
    tk_assert(q != NULL, "second allocation failed");
    tk_assert(q == p, "allocator did not reuse the just-freed block");
    fill_bytes(q, 128, 0x5a);
    myfree(q);
}

UnitTest(m5_large_allocation) {
    size_t size = 200000;
    void *p = mymalloc(size);
    tk_assert(p != NULL, "large allocation failed");
    tk_assert(((uintptr_t)p & 7) == 0, "large allocation is not aligned");
    fill_bytes(p, size, 0x3c);
    check_bytes(p, size, 0x3c);
    myfree(p);
}

enum {
    THREADS = 8,
    SLOTS = 64,
    OPS = 20000,
};

struct worker_arg {
    int id;
    void *ptrs[SLOTS];
    size_t sizes[SLOTS];
    unsigned char marks[SLOTS];
};

static uint32_t next_rand(uint32_t *state) {
    *state = *state * 1103515245u + 12345u;
    return *state;
}

static void *worker(void *arg) {
    struct worker_arg *wa = arg;
    uint32_t rng = 0x9e3779b9u ^ (uint32_t)wa->id;

    for (int op = 0; op < OPS; op++) {
        int slot = (int)(next_rand(&rng) % SLOTS);
        if (wa->ptrs[slot] == NULL) {
            size_t size = (size_t)(next_rand(&rng) % 2048) + 1;
            unsigned char mark = (unsigned char)(0x40 + wa->id + slot);
            void *p = mymalloc(size);
            tk_assert(p != NULL, "thread %d allocation failed", wa->id);
            tk_assert(((uintptr_t)p & 7) == 0,
                      "thread %d got unaligned pointer %p", wa->id, p);
            fill_bytes(p, size, mark);
            wa->ptrs[slot] = p;
            wa->sizes[slot] = size;
            wa->marks[slot] = mark;
        } else {
            check_bytes(wa->ptrs[slot], wa->sizes[slot], wa->marks[slot]);
            myfree(wa->ptrs[slot]);
            wa->ptrs[slot] = NULL;
            wa->sizes[slot] = 0;
        }
    }

    for (int i = 0; i < SLOTS; i++) {
        if (wa->ptrs[i] != NULL) {
            check_bytes(wa->ptrs[i], wa->sizes[i], wa->marks[i]);
            myfree(wa->ptrs[i]);
            wa->ptrs[i] = NULL;
        }
    }

    return NULL;
}

UnitTest(m5_concurrent_alloc_free_stress) {
    pthread_t threads[THREADS];
    struct worker_arg args[THREADS] = {};

    for (int i = 0; i < THREADS; i++) {
        args[i].id = i;
        int rc = pthread_create(&threads[i], NULL, worker, &args[i]);
        tk_assert(rc == 0, "pthread_create failed: %d", rc);
    }

    for (int i = 0; i < THREADS; i++) {
        int rc = pthread_join(threads[i], NULL);
        tk_assert(rc == 0, "pthread_join failed: %d", rc);
    }
}
