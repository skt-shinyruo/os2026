// This is just a demonstration of how to write test cases.
// Write good test cases by yourself ;)

#include <mymalloc.h>
#include <pthread.h>
#include <sched.h>
#include <testkit.h>

typedef struct {
    unsigned char *ptr;
    size_t size;
    unsigned char seed;
} tracked_block_t;

static void fill_pattern(unsigned char *ptr, size_t size, unsigned char seed) {
    for (size_t i = 0; i < size; i++) {
        ptr[i] = (unsigned char)(seed + i * 17u);
    }
}

static void assert_pattern(const tracked_block_t *block, const char *context) {
    for (size_t i = 0; i < block->size; i++) {
        unsigned char expected = (unsigned char)(block->seed + i * 17u);
        tk_assert(block->ptr[i] == expected,
                  "%s: byte %zu corrupted (expected %u, got %u)", context, i,
                  (unsigned)expected, (unsigned)block->ptr[i]);
    }
}

static void assert_no_overlap(const tracked_block_t *blocks, size_t count,
                              const char *context) {
    for (size_t i = 0; i < count; i++) {
        uintptr_t left_i = (uintptr_t)blocks[i].ptr;
        uintptr_t right_i = left_i + blocks[i].size;

        for (size_t j = i + 1; j < count; j++) {
            uintptr_t left_j = (uintptr_t)blocks[j].ptr;
            uintptr_t right_j = left_j + blocks[j].size;

            tk_assert(right_i <= left_j || right_j <= left_i,
                      "%s: live ranges overlap: block %zu [%p, %p) vs block "
                      "%zu [%p, %p)",
                      context, i, (void *)left_i, (void *)right_i, j,
                      (void *)left_j, (void *)right_j);
        }
    }
}

static void assert_blocks_healthy(const tracked_block_t *blocks, size_t count,
                                  const char *context) {
    assert_no_overlap(blocks, count, context);
    for (size_t i = 0; i < count; i++) {
        assert_pattern(&blocks[i], context);
    }
}

static tracked_block_t alloc_block(size_t size, unsigned char seed,
                                   const char *context) {
    tracked_block_t block = {
        .ptr = mymalloc(size),
        .size = size,
        .seed = seed,
    };

    tk_assert(block.ptr != NULL, "%s: mymalloc(%zu) returned NULL", context,
              size);
    tk_assert((uintptr_t)block.ptr % ALIGNMENT == 0,
              "%s: returned pointer %p is not %d-byte aligned", context,
              (void *)block.ptr, ALIGNMENT);
    fill_pattern(block.ptr, block.size, block.seed);
    return block;
}

SystemTest(trivial, ((const char *[]){})) {
    int *p1 = mymalloc(4);
    tk_assert(p1 != NULL, "malloc should not return NULL");
    *p1 = 1024;

    int *p2 = mymalloc(4);
    tk_assert(p2 != NULL, "malloc should not return NULL");
    *p2 = 2048;

    tk_assert(p1 != p2, "malloc should return different pointers");
    tk_assert(*p1 * 2 == *p2, "value check should pass");

    myfree(p1);
    myfree(p2);
}

SystemTest(vmalloc, ((const char *[]){})) {
    void *p1 = vmalloc(NULL, 4096);
    tk_assert(p1 != NULL, "vmalloc should not return NULL");
    tk_assert((uintptr_t)p1 % 4096 == 0,
              "vmalloc should return page-aligned address");

    void *p2 = vmalloc(NULL, 8192);
    tk_assert(p2 != NULL, "vmalloc should not return NULL");
    tk_assert((uintptr_t)p2 % 4096 == 0,
              "vmalloc should return page-aligned address");
    tk_assert(p1 != p2, "vmalloc should return different pointers");

    vmfree(p1, 4096);
    vmfree(p2, 8192);
}

SystemTest(zero_size_and_alignment, ((const char *[]){})) {
    static const size_t sizes[] = {1,  2,  3,  7,  8,  9,  15,  16,  17,
                                   31, 32, 33, 63, 64, 65, 127, 128, 255};
    tracked_block_t blocks[sizeof(sizes) / sizeof(sizes[0])];

    tk_assert(mymalloc(0) == NULL, "mymalloc(0) should return NULL");

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        blocks[i] = alloc_block(sizes[i], (unsigned char)(0x11 + i),
                                "zero_size_and_alignment");
        assert_blocks_healthy(blocks, i + 1, "zero_size_and_alignment");
    }

    for (size_t i = sizeof(sizes) / sizeof(sizes[0]); i > 0; i--) {
        myfree(blocks[i - 1].ptr);
    }
}

SystemTest(interleaved_free_and_reallocate, ((const char *[]){})) {
    static const size_t original_sizes[] = {24, 40, 56, 72, 88, 104, 120, 136};
    tracked_block_t live[sizeof(original_sizes) / sizeof(original_sizes[0])];

    for (size_t i = 0; i < sizeof(original_sizes) / sizeof(original_sizes[0]);
         i++) {
        live[i] = alloc_block(original_sizes[i], (unsigned char)(0x31 + i),
                              "interleaved_free_and_reallocate");
    }
    assert_blocks_healthy(live, sizeof(live) / sizeof(live[0]),
                          "interleaved_free_and_reallocate");

    for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i += 2) {
        myfree(live[i].ptr);
        live[i] =
            alloc_block(original_sizes[i] / 2 + 1, (unsigned char)(0x61 + i),
                        "interleaved_free_and_reallocate");
        assert_blocks_healthy(live, sizeof(live) / sizeof(live[0]),
                              "interleaved_free_and_reallocate");
    }

    for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i++) {
        myfree(live[i].ptr);
    }
}

SystemTest(large_allocations_preserve_contents, ((const char *[]){})) {
    static const size_t sizes[] = {511, 4096, 4097, 8192, 12345};
    tracked_block_t blocks[sizeof(sizes) / sizeof(sizes[0])];

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        blocks[i] = alloc_block(sizes[i], (unsigned char)(0x81 + i),
                                "large_allocations_preserve_contents");
    }
    assert_blocks_healthy(blocks, sizeof(blocks) / sizeof(blocks[0]),
                          "large_allocations_preserve_contents");

    for (size_t i = sizeof(blocks) / sizeof(blocks[0]); i > 0; i--) {
        myfree(blocks[i - 1].ptr);
    }
}

enum {
    THREAD_COUNT = 4,
    BLOCKS_PER_THREAD = 32,
};

typedef struct {
    pthread_barrier_t *barrier;

    tracked_block_t blocks[BLOCKS_PER_THREAD];
    int tid;
} thread_ctx_t;

static void *thread_alloc_blocks(void *arg) {
    thread_ctx_t *ctx = arg;
    int rc = pthread_barrier_wait(ctx->barrier);
    tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
              "pthread_barrier_wait should succeed");

    for (size_t i = 0; i < BLOCKS_PER_THREAD; i++) {
        size_t size = 24 + (size_t)ctx->tid * 8 + (i % 5) * 7;
        ctx->blocks[i] =
            alloc_block(size, (unsigned char)(0xA0 + ctx->tid * 13 + i),
                        "concurrent_live_allocations");
        if ((i & 3u) == 0) {
            sched_yield();
        }
    }
    return NULL;
}

SystemTest(concurrent_live_allocations, ((const char *[]){})) {
    pthread_barrier_t barrier;
    pthread_t threads[THREAD_COUNT];
    thread_ctx_t ctx[THREAD_COUNT];
    tracked_block_t all_blocks[THREAD_COUNT * BLOCKS_PER_THREAD];

    tk_assert(pthread_barrier_init(&barrier, NULL, THREAD_COUNT + 1) == 0,
              "pthread_barrier_init should succeed");

    for (int i = 0; i < THREAD_COUNT; i++) {
        ctx[i] = (thread_ctx_t){
            .barrier = &barrier,
            .tid = i,
        };
        tk_assert(pthread_create(&threads[i], NULL, thread_alloc_blocks,
                                 &ctx[i]) == 0,
                  "pthread_create should succeed");
    }

    {
        int rc = pthread_barrier_wait(&barrier);
        tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
                  "pthread_barrier_wait should succeed");
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0,
                  "pthread_join should succeed");
    }
    tk_assert(pthread_barrier_destroy(&barrier) == 0,
              "pthread_barrier_destroy should succeed");

    for (size_t i = 0; i < THREAD_COUNT; i++) {
        for (size_t j = 0; j < BLOCKS_PER_THREAD; j++) {
            all_blocks[i * BLOCKS_PER_THREAD + j] = ctx[i].blocks[j];
        }
    }

    assert_blocks_healthy(all_blocks,
                          sizeof(all_blocks) / sizeof(all_blocks[0]),
                          "concurrent_live_allocations");

    for (size_t i = 0; i < sizeof(all_blocks) / sizeof(all_blocks[0]); i++) {
        myfree(all_blocks[i].ptr);
    }
}
