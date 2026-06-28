// This is a deterministic regression suite for mymalloc/myfree.
// The goal is broad local coverage before facing the OJ workloads.

#include <mymalloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <testkit.h>

#define ARRAY_LEN(arr) (sizeof(arr) / sizeof((arr)[0]))
#define PAGE_SIZE 4096u
#define FOOTPRINT_SLACK_PAGES 8u
#define MAX_TRACKED_PAGES 4096u
#define MYMALLOC_STRESS_ENV "MYMALLOC_TEST_STRESS"

static bool stress_tests_enabled(void) {
    const char *value = getenv(MYMALLOC_STRESS_ENV);
    return value != NULL && value[0] != '\0' && value[0] != '0';
}

#define StressSystemTest(name, argv_, ...)                                      \
    StressTestCase(name, struct tk_result *result, stest,                      \
                   .argc = sizeof(argv_) / sizeof(void *),                     \
                   .argv = (const char **)argv_, __VA_ARGS__)

#define StressTestCase(name_, body_arg, test, ...)                             \
    static void TK_UNIQUE_NAME(name_)(body_arg);                               \
    __attribute__((constructor)) void TK_UNIQUE_NAME(reg##name_)() {           \
        void tk_add_test(struct tk_testcase t);                                \
        if (stress_tests_enabled()) {                                          \
            tk_add_test((struct tk_testcase){                                  \
                .enabled = 1,                                                  \
                .name = #name_,                                                \
                .loc = __FILE__ ":" TK_TOSTRING(__LINE__),                    \
                .test = TK_UNIQUE_NAME(name_),                                 \
                __VA_ARGS__                                                    \
            });                                                               \
        }                                                                     \
    }                                                                         \
    static void TK_UNIQUE_NAME(name_)(body_arg)

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

static void clear_block(tracked_block_t *block) {
    *block = (tracked_block_t){0};
}

static void free_block(tracked_block_t *block) {
    if (block->ptr != NULL) {
        myfree(block->ptr);
        clear_block(block);
    }
}

static void free_all_blocks(tracked_block_t *blocks, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free_block(&blocks[i]);
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

static void assert_live_blocks_healthy(const tracked_block_t *blocks,
                                       size_t count, const char *context) {
    size_t live_count = 0;
    tracked_block_t live[count == 0 ? 1 : count];

    for (size_t i = 0; i < count; i++) {
        if (blocks[i].ptr != NULL) {
            live[live_count++] = blocks[i];
        }
    }

    if (live_count > 0) {
        assert_blocks_healthy(live, live_count, context);
    }
}

static void assert_guard_sample(const tracked_block_t *blocks, size_t count,
                                const char *context) {
    tk_assert(count > 0, "%s: guard sample requires at least one block",
              context);
    assert_pattern(&blocks[0], context);
    assert_pattern(&blocks[count / 2], context);
    assert_pattern(&blocks[count - 1], context);
}

static int barrier_wait_ok(pthread_barrier_t *barrier, const char *context) {
    int rc = pthread_barrier_wait(barrier);
    tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
              "%s: pthread_barrier_wait should succeed", context);
    return rc;
}

static size_t count_touched_pages(const tracked_block_t *blocks, size_t count,
                                  const char *context) {
    uintptr_t seen_pages[MAX_TRACKED_PAGES];
    size_t seen_count = 0;

    for (size_t i = 0; i < count; i++) {
        if (blocks[i].ptr == NULL || blocks[i].size == 0) {
            continue;
        }

        uintptr_t first = (uintptr_t)blocks[i].ptr / PAGE_SIZE;
        uintptr_t last =
            ((uintptr_t)blocks[i].ptr + blocks[i].size - 1u) / PAGE_SIZE;

        for (uintptr_t page = first; page <= last; page++) {
            bool already_seen = false;
            for (size_t j = 0; j < seen_count; j++) {
                if (seen_pages[j] == page) {
                    already_seen = true;
                    break;
                }
            }

            if (!already_seen) {
                tk_assert(seen_count < MAX_TRACKED_PAGES,
                          "%s: page tracking overflow", context);
                seen_pages[seen_count++] = page;
            }
        }
    }

    return seen_count;
}

static void assert_memory_footprint_within(const tracked_block_t *blocks,
                                           size_t count, size_t multiplier,
                                           size_t slack_pages,
                                           const char *context) {
    size_t requested_total = 0;

    for (size_t i = 0; i < count; i++) {
        if (blocks[i].ptr != NULL) {
            requested_total += blocks[i].size;
        }
    }

    if (requested_total == 0) {
        return;
    }

    size_t touched_pages = count_touched_pages(blocks, count, context);
    size_t footprint = touched_pages * PAGE_SIZE;
    size_t bound = requested_total * multiplier + slack_pages * PAGE_SIZE;

    tk_assert(footprint <= bound,
              "%s: live footprint too large (%zu bytes across %zu pages for "
              "%zu requested bytes; bound=%zu)",
              context, footprint, touched_pages, requested_total, bound);
}

static uint32_t next_u32(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static size_t select_small_size(uint32_t value) {
    static const size_t sizes[] = {
        1,  2,  3,  5,  7,  8,  9,  12, 15, 16, 17, 24,
        31, 32, 33, 47, 48, 49, 63, 64, 65, 95, 96, 127,
    };

    return sizes[value % ARRAY_LEN(sizes)];
}

static size_t select_mixed_size(uint32_t value) {
    static const size_t sizes[] = {
        1,    7,    8,    9,    15,   16,   17,   31,
        32,   33,   63,   64,   65,   127,  128,  129,
        255,  256,  257,  511,  512,  513,  1023, 1024,
        1025, 2047, 2048, 2049, 4095, 4096, 4097,
    };

    return sizes[value % ARRAY_LEN(sizes)];
}

static void run_slot_churn(tracked_block_t *slots, size_t slot_count,
                           size_t steps, uint32_t seed, bool mixed_sizes,
                           const char *context) {
    for (size_t step = 0; step < steps; step++) {
        size_t slot = next_u32(&seed) % slot_count;
        uint32_t action = next_u32(&seed);

        if (slots[slot].ptr != NULL && (action & 3u) == 0) {
            assert_pattern(&slots[slot], context);
            free_block(&slots[slot]);
        } else {
            if (slots[slot].ptr != NULL) {
                assert_pattern(&slots[slot], context);
                free_block(&slots[slot]);
            }

            size_t size = mixed_sizes ? select_mixed_size(action)
                                      : select_small_size(action);
            slots[slot] = alloc_block(
                size, (unsigned char)(0x40u + (step * 11u + slot * 7u)),
                context);
        }

        if ((step & 7u) == 0) {
            assert_live_blocks_healthy(slots, slot_count, context);
        }
        if (!mixed_sizes && (step & 15u) == 0) {
            assert_memory_footprint_within(slots, slot_count, 4,
                                           FOOTPRINT_SLACK_PAGES, context);
        }
    }

    assert_live_blocks_healthy(slots, slot_count, context);
}

static void run_tail_pressure(tracked_block_t *prefix, size_t prefix_count,
                              tracked_block_t *hot, size_t iterations,
                              size_t guard_interval, size_t yield_interval,
                              const char *context) {
    for (size_t i = 0; i < iterations; i++) {
        free_block(hot);
        *hot = alloc_block(64, (unsigned char)(0x50u + (i & 0x7fu)), context);

        if (guard_interval != 0 && (i % guard_interval) == 0) {
            assert_guard_sample(prefix, prefix_count, context);
        }

        if (yield_interval != 0 && (i % yield_interval) == 0) {
            sched_yield();
        }
    }

    assert_guard_sample(prefix, prefix_count, context);
}

static size_t phase_fragment_size(int phase, int tid, size_t index) {
    if ((index % 12u) == 0) {
        return 4095u + (size_t)(phase & 1u);
    }
    if ((index % 12u) == 1) {
        return 256u + (size_t)tid * 8u + (size_t)phase * 16u;
    }
    if ((index % 12u) == 2) {
        return 511u + (size_t)(tid + phase) * 3u;
    }
    if ((index % 12u) == 3) {
        return 64u + (index % 7u) * 13u;
    }

    return 24u + (index * 29u + (size_t)phase * 11u + (size_t)tid * 7u) % 192u;
}

static size_t burst_hot_cold_size(uint32_t state, size_t round, size_t index) {
    if ((index % 10u) == 0) {
        static const size_t sizes[] = {127, 128, 129, 255, 256, 257, 511, 512,
                                       513, 1023, 1024, 1025};
        return sizes[(state + (uint32_t)round + (uint32_t)index) %
                     ARRAY_LEN(sizes)];
    }
    if ((index % 10u) == 1) {
        return 64u + ((round + index) % 11u) * 19u;
    }
    if ((index % 10u) == 2) {
        return 32u + ((round + index) % 9u) * 11u;
    }

    return select_small_size(state + (uint32_t)round + (uint32_t)index);
}

static size_t shared_pool_size(size_t round, size_t index, int tid) {
    if ((index % 8u) == 0) {
        return 128u + (round % 5u) * 32u + (size_t)tid * 8u;
    }
    if ((index % 8u) == 1) {
        return 256u + (round % 4u) * 64u;
    }
    if ((index % 8u) == 2) {
        return 4095u + (round & 1u);
    }
    if ((index % 8u) == 3) {
        return 64u + ((round + index) % 9u) * 19u;
    }

    return select_mixed_size((uint32_t)(round * 97u + index * 13u +
                                         (size_t)tid * 7u));
}

static unsigned char shared_pool_seed(size_t round, size_t index, int tid) {
    return (unsigned char)(0x20u + round * 5u + index * 3u + tid * 11u);
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
    tk_assert((uintptr_t)p1 % PAGE_SIZE == 0,
              "vmalloc should return page-aligned address");

    void *p2 = vmalloc(NULL, 8192);
    tk_assert(p2 != NULL, "vmalloc should not return NULL");
    tk_assert((uintptr_t)p2 % PAGE_SIZE == 0,
              "vmalloc should return page-aligned address");
    tk_assert(p1 != p2, "vmalloc should return different pointers");

    vmfree(p1, 4096);
    vmfree(p2, 8192);
}

SystemTest(zero_size_and_alignment, ((const char *[]){})) {
    static const size_t sizes[] = {1,  2,  3,   7,   8,   9,   15,  16,  17,
                                   31, 32, 33,  63,  64,  65,  127, 128, 255,
                                   256, 257, 511, 512, 513};
    tracked_block_t blocks[ARRAY_LEN(sizes)];

    tk_assert(mymalloc(0) == NULL, "mymalloc(0) should return NULL");

    for (size_t i = 0; i < ARRAY_LEN(sizes); i++) {
        blocks[i] = alloc_block(sizes[i], (unsigned char)(0x11 + i),
                                "zero_size_and_alignment");
        assert_live_blocks_healthy(blocks, i + 1, "zero_size_and_alignment");
    }

    free_all_blocks(blocks, ARRAY_LEN(blocks));
}

SystemTest(boundary_size_matrix, ((const char *[]){})) {
    static const size_t sizes[] = {
        1,    2,    3,    4,    5,    7,    8,    9,    15,
        16,   17,   31,   32,   33,   63,   64,   65,   127,
        128,  129,  255,  256,  257,  511,  512,  513,  1023,
        1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097,
    };
    tracked_block_t blocks[ARRAY_LEN(sizes)];

    for (size_t i = 0; i < ARRAY_LEN(sizes); i++) {
        blocks[i] = alloc_block(sizes[i], (unsigned char)(0x30u + i),
                                "boundary_size_matrix");
    }
    assert_live_blocks_healthy(blocks, ARRAY_LEN(blocks),
                               "boundary_size_matrix");

    for (size_t i = 0; i < ARRAY_LEN(blocks); i += 2) {
        free_block(&blocks[i]);
    }
    assert_live_blocks_healthy(blocks, ARRAY_LEN(blocks),
                               "boundary_size_matrix");

    for (size_t i = 0; i < ARRAY_LEN(blocks); i += 2) {
        size_t replacement = sizes[ARRAY_LEN(sizes) - 1 - i];
        blocks[i] = alloc_block(replacement,
                                (unsigned char)(0x70u + i),
                                "boundary_size_matrix");
    }
    assert_live_blocks_healthy(blocks, ARRAY_LEN(blocks),
                               "boundary_size_matrix");

    free_all_blocks(blocks, ARRAY_LEN(blocks));
}

SystemTest(dense_small_allocations_have_reasonable_footprint,
           ((const char *[]){})) {
    enum { DENSE_COUNT = 128 };
    tracked_block_t blocks[DENSE_COUNT];

    for (size_t i = 0; i < DENSE_COUNT; i++) {
        size_t size = 1u + (i % 16u);
        blocks[i] = alloc_block(size, (unsigned char)(0x90u + i),
                                "dense_small_allocations_have_reasonable_footprint");
    }

    assert_live_blocks_healthy(
        blocks, DENSE_COUNT, "dense_small_allocations_have_reasonable_footprint");
    assert_memory_footprint_within(
        blocks, DENSE_COUNT, 4, FOOTPRINT_SLACK_PAGES,
        "dense_small_allocations_have_reasonable_footprint");

    free_all_blocks(blocks, DENSE_COUNT);
}

SystemTest(phased_fragmentation_preserves_contents, ((const char *[]){})) {
    enum { BLOCK_COUNT = 48 };
    tracked_block_t blocks[BLOCK_COUNT];

    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        size_t size = 24u + (i % 9u) * 13u;
        blocks[i] = alloc_block(size, (unsigned char)(0xA0u + i),
                                "phased_fragmentation_preserves_contents");
    }
    assert_live_blocks_healthy(blocks, BLOCK_COUNT,
                               "phased_fragmentation_preserves_contents");

    for (size_t i = 0; i < BLOCK_COUNT; i += 3) {
        free_block(&blocks[i]);
    }
    assert_live_blocks_healthy(blocks, BLOCK_COUNT,
                               "phased_fragmentation_preserves_contents");

    for (size_t i = 0; i < BLOCK_COUNT; i += 3) {
        size_t size = 19u + (i % 7u) * 11u;
        blocks[i] = alloc_block(size, (unsigned char)(0xD0u + i),
                                "phased_fragmentation_preserves_contents");
    }
    assert_live_blocks_healthy(blocks, BLOCK_COUNT,
                               "phased_fragmentation_preserves_contents");

    for (size_t i = 1; i < BLOCK_COUNT; i += 4) {
        free_block(&blocks[i]);
    }

    for (size_t i = 1; i < BLOCK_COUNT; i += 4) {
        size_t size = 48u + (i % 5u) * 17u;
        blocks[i] = alloc_block(size, (unsigned char)(0xE0u + i),
                                "phased_fragmentation_preserves_contents");
    }
    assert_live_blocks_healthy(blocks, BLOCK_COUNT,
                               "phased_fragmentation_preserves_contents");

    free_all_blocks(blocks, BLOCK_COUNT);
}

SystemTest(release_everything_and_rebuild, ((const char *[]){})) {
    enum { BLOCK_COUNT = 40 };
    tracked_block_t first_round[BLOCK_COUNT];
    tracked_block_t second_round[BLOCK_COUNT];

    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        size_t size = 8u + (i * 37u) % 769u;
        first_round[i] = alloc_block(size, (unsigned char)(0x21u + i),
                                     "release_everything_and_rebuild");
    }
    assert_live_blocks_healthy(first_round, BLOCK_COUNT,
                               "release_everything_and_rebuild");

    for (size_t i = BLOCK_COUNT; i > 0; i--) {
        free_block(&first_round[i - 1]);
    }

    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        size_t reversed = BLOCK_COUNT - 1u - i;
        size_t size = 16u + (reversed * 53u) % 1025u;
        second_round[i] = alloc_block(size, (unsigned char)(0x61u + i),
                                      "release_everything_and_rebuild");
    }
    assert_live_blocks_healthy(second_round, BLOCK_COUNT,
                               "release_everything_and_rebuild");

    free_all_blocks(second_round, BLOCK_COUNT);
}

SystemTest(large_allocations_preserve_contents, ((const char *[]){})) {
    static const size_t sizes[] = {511, 4095, 4096, 4097, 8192, 12345};
    tracked_block_t blocks[ARRAY_LEN(sizes)];

    for (size_t i = 0; i < ARRAY_LEN(sizes); i++) {
        blocks[i] = alloc_block(sizes[i], (unsigned char)(0x81 + i),
                                "large_allocations_preserve_contents");
    }
    assert_live_blocks_healthy(blocks, ARRAY_LEN(blocks),
                               "large_allocations_preserve_contents");

    free_all_blocks(blocks, ARRAY_LEN(blocks));
}

SystemTest(oversized_requests_fail_cleanly, ((const char *[]){})) {
    tracked_block_t guard = alloc_block(64, 0xD1,
                                        "oversized_requests_fail_cleanly");

    tk_assert(mymalloc(SIZE_MAX) == NULL,
              "mymalloc(SIZE_MAX) should return NULL");
    tk_assert(mymalloc(SIZE_MAX - HEADER_SIZE) == NULL,
              "mymalloc(SIZE_MAX - HEADER_SIZE) should return NULL");

    assert_pattern(&guard, "oversized_requests_fail_cleanly");

    tracked_block_t after = alloc_block(64, 0xD2,
                                        "oversized_requests_fail_cleanly");
    tracked_block_t live[] = {guard, after};
    assert_live_blocks_healthy(live, ARRAY_LEN(live),
                               "oversized_requests_fail_cleanly");

    free_all_blocks(live, ARRAY_LEN(live));
}

SystemTest(split_free_block_into_many_smaller_blocks,
           ((const char *[]){})) {
    enum { SMALL_COUNT = 64 };
    tracked_block_t guard = alloc_block(256, 0xB1,
                                        "split_free_block_into_many_smaller_blocks");
    tracked_block_t slab = alloc_block(16384, 0xB2,
                                       "split_free_block_into_many_smaller_blocks");
    tracked_block_t tail = alloc_block(256, 0xB3,
                                       "split_free_block_into_many_smaller_blocks");
    tracked_block_t smalls[SMALL_COUNT];
    tracked_block_t live[SMALL_COUNT + 2];

    myfree(slab.ptr);
    clear_block(&slab);

    for (size_t i = 0; i < SMALL_COUNT; i++) {
        size_t size = 128u + (i % 4u) * 8u;
        smalls[i] = alloc_block(size, (unsigned char)(0xC0u + i),
                                "split_free_block_into_many_smaller_blocks");
    }

    live[0] = guard;
    for (size_t i = 0; i < SMALL_COUNT; i++) {
        live[i + 1] = smalls[i];
    }
    live[SMALL_COUNT + 1] = tail;

    assert_live_blocks_healthy(live, ARRAY_LEN(live),
                               "split_free_block_into_many_smaller_blocks");
    assert_memory_footprint_within(live, ARRAY_LEN(live), 4,
                                   FOOTPRINT_SLACK_PAGES,
                                   "split_free_block_into_many_smaller_blocks");

    free_all_blocks(live, ARRAY_LEN(live));
}

SystemTest(reuse_after_small_blocks_are_merged, ((const char *[]){})) {
    enum { TINY_COUNT = 64, MEDIUM_COUNT = 32 };
    tracked_block_t tiny[TINY_COUNT];
    tracked_block_t medium[MEDIUM_COUNT];

    for (size_t i = 0; i < TINY_COUNT; i++) {
        size_t size = 96u + (i % 4u) * 8u;
        tiny[i] = alloc_block(size, (unsigned char)(0xE0u + i),
                              "reuse_after_small_blocks_are_merged");
    }
    assert_live_blocks_healthy(tiny, ARRAY_LEN(tiny),
                               "reuse_after_small_blocks_are_merged");

    free_all_blocks(tiny, ARRAY_LEN(tiny));

    for (size_t i = 0; i < MEDIUM_COUNT; i++) {
        size_t size = 512u + (i % 3u) * 16u;
        medium[i] = alloc_block(size, (unsigned char)(0xF0u + i),
                                "reuse_after_small_blocks_are_merged");
    }

    assert_live_blocks_healthy(medium, ARRAY_LEN(medium),
                               "reuse_after_small_blocks_are_merged");
    assert_memory_footprint_within(medium, ARRAY_LEN(medium), 4,
                                   FOOTPRINT_SLACK_PAGES,
                                   "reuse_after_small_blocks_are_merged");

    free_all_blocks(medium, ARRAY_LEN(medium));
}

SystemTest(deterministic_slot_churn_small, ((const char *[]){})) {
    enum { SLOT_COUNT = 48, STEP_COUNT = 384 };
    tracked_block_t slots[SLOT_COUNT] = {0};

    run_slot_churn(slots, SLOT_COUNT, STEP_COUNT, 0xC0FFEE11u, false,
                   "deterministic_slot_churn_small");
    assert_memory_footprint_within(slots, SLOT_COUNT, 4, FOOTPRINT_SLACK_PAGES,
                                   "deterministic_slot_churn_small");

    free_all_blocks(slots, SLOT_COUNT);
}

SystemTest(deterministic_slot_churn_mixed, ((const char *[]){})) {
    enum { SLOT_COUNT = 32, STEP_COUNT = 320 };
    tracked_block_t slots[SLOT_COUNT] = {0};

    run_slot_churn(slots, SLOT_COUNT, STEP_COUNT, 0x5EED1234u, true,
                   "deterministic_slot_churn_mixed");

    free_all_blocks(slots, SLOT_COUNT);
}

StressSystemTest(single_thread_tail_pressure, ((const char *[]){})) {
    enum {
        PREFIX_COUNT = 1024,
        ITERATIONS = 120000,
    };

    tracked_block_t prefix[PREFIX_COUNT];
    tracked_block_t hot;

    for (size_t i = 0; i < PREFIX_COUNT; i++) {
        prefix[i] = alloc_block(64, (unsigned char)(0xB0u + i),
                                "single_thread_tail_pressure");
    }
    assert_guard_sample(prefix, PREFIX_COUNT, "single_thread_tail_pressure");

    hot = alloc_block(64, 0xEF, "single_thread_tail_pressure");
    run_tail_pressure(prefix, PREFIX_COUNT, &hot, ITERATIONS, 4096, 8192,
                      "single_thread_tail_pressure");

    free_block(&hot);
    free_all_blocks(prefix, PREFIX_COUNT);
}

typedef struct {
    pthread_barrier_t *barrier;
    tracked_block_t prefix[256];
    tracked_block_t hot;
    int tid;
} tail_pressure_thread_ctx_t;

static void *thread_tail_pressure(void *arg) {
    tail_pressure_thread_ctx_t *ctx = arg;

    for (size_t i = 0; i < ARRAY_LEN(ctx->prefix); i++) {
        ctx->prefix[i] = alloc_block(
            64, (unsigned char)(0x30u + ctx->tid * 17u + i),
            "parallel_tail_pressure");
    }
    assert_guard_sample(ctx->prefix, ARRAY_LEN(ctx->prefix),
                        "parallel_tail_pressure");

    ctx->hot = alloc_block(64, (unsigned char)(0x90u + ctx->tid),
                           "parallel_tail_pressure");

    int rc = pthread_barrier_wait(ctx->barrier);
    tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
              "pthread_barrier_wait should succeed");

    run_tail_pressure(ctx->prefix, ARRAY_LEN(ctx->prefix), &ctx->hot, 40000,
                      2048, 4096, "parallel_tail_pressure");
    return NULL;
}

StressSystemTest(parallel_tail_pressure, ((const char *[]){})) {
    enum {
        THREAD_COUNT = 8,
    };

    pthread_barrier_t barrier;
    pthread_t threads[THREAD_COUNT];
    tail_pressure_thread_ctx_t ctx[THREAD_COUNT];

    tk_assert(pthread_barrier_init(&barrier, NULL, THREAD_COUNT + 1) == 0,
              "pthread_barrier_init should succeed");

    for (int i = 0; i < THREAD_COUNT; i++) {
        ctx[i] = (tail_pressure_thread_ctx_t){
            .barrier = &barrier,
            .tid = i,
        };
        tk_assert(pthread_create(&threads[i], NULL, thread_tail_pressure,
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

    for (int i = 0; i < THREAD_COUNT; i++) {
        assert_guard_sample(ctx[i].prefix, ARRAY_LEN(ctx[i].prefix),
                            "parallel_tail_pressure");
        free_block(&ctx[i].hot);
        free_all_blocks(ctx[i].prefix, ARRAY_LEN(ctx[i].prefix));
    }
}

SystemTest(burst_hot_cold_allocations, ((const char *[]){})) {
    enum {
        WARM_COUNT = 192,
        BURST_ROUNDS = 128,
        HOT_SLOTS = 24,
    };

    tracked_block_t warm[WARM_COUNT];
    tracked_block_t hot[HOT_SLOTS] = {0};

    for (size_t i = 0; i < WARM_COUNT; i++) {
        size_t size = 32u + (i % 13u) * 17u;
        warm[i] = alloc_block(size, (unsigned char)(0x10u + i),
                              "burst_hot_cold_allocations");
    }
    assert_live_blocks_healthy(warm, ARRAY_LEN(warm),
                               "burst_hot_cold_allocations");

    for (size_t round = 0; round < BURST_ROUNDS; round++) {
        for (size_t i = 0; i < HOT_SLOTS; i++) {
            free_block(&hot[i]);
            size_t size = burst_hot_cold_size(0xA5A50000u, round, i);
            hot[i] = alloc_block(size, (unsigned char)(0x80u + round + i),
                                 "burst_hot_cold_allocations");
        }

        if ((round & 7u) == 0) {
            assert_live_blocks_healthy(warm, ARRAY_LEN(warm),
                                       "burst_hot_cold_allocations");
            assert_live_blocks_healthy(hot, ARRAY_LEN(hot),
                                       "burst_hot_cold_allocations");
            assert_memory_footprint_within(warm, ARRAY_LEN(warm), 4,
                                           FOOTPRINT_SLACK_PAGES,
                                           "burst_hot_cold_allocations");
        }

        if ((round & 3u) == 0) {
            sched_yield();
        }
    }

    free_all_blocks(hot, ARRAY_LEN(hot));
    free_all_blocks(warm, ARRAY_LEN(warm));
}

typedef struct {
    pthread_barrier_t *barrier;
    tracked_block_t live[96];
    tracked_block_t scratch[32];
    int tid;
} phase_pressure_thread_ctx_t;

static void phase_pressure_init_live(phase_pressure_thread_ctx_t *ctx,
                                     int phase) {
    for (size_t i = 0; i < ARRAY_LEN(ctx->live); i++) {
        size_t size = phase_fragment_size(phase, ctx->tid, i);
        ctx->live[i] = alloc_block(size,
                                   (unsigned char)(0x20u + ctx->tid * 11u + i +
                                                   phase * 3u),
                                   "parallel_phase_pressure");
    }
}

static void phase_pressure_rotate(phase_pressure_thread_ctx_t *ctx, int phase) {
    for (size_t i = 0; i < ARRAY_LEN(ctx->scratch); i++) {
        clear_block(&ctx->scratch[i]);
    }

    for (size_t i = 0; i < ARRAY_LEN(ctx->live); i += 2) {
        free_block(&ctx->live[i]);
    }

    for (size_t i = 0; i < ARRAY_LEN(ctx->scratch); i++) {
        size_t size = phase_fragment_size(phase + 1, ctx->tid, i + 7u);
        ctx->scratch[i] =
            alloc_block(size, (unsigned char)(0x60u + ctx->tid * 9u + i),
                        "parallel_phase_pressure");
    }

    for (size_t i = 0; i < ARRAY_LEN(ctx->live); i += 2) {
        size_t size = phase_fragment_size(phase + 2, ctx->tid, i + 13u);
        ctx->live[i] =
            alloc_block(size, (unsigned char)(0xA0u + ctx->tid * 7u + i),
                        "parallel_phase_pressure");
    }

    assert_live_blocks_healthy(ctx->live, ARRAY_LEN(ctx->live),
                               "parallel_phase_pressure");
    assert_live_blocks_healthy(ctx->scratch, ARRAY_LEN(ctx->scratch),
                               "parallel_phase_pressure");
}

static void *thread_phase_pressure(void *arg) {
    phase_pressure_thread_ctx_t *ctx = arg;

    int rc = pthread_barrier_wait(ctx->barrier);
    tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
              "pthread_barrier_wait should succeed");

    for (int phase = 0; phase < 6; phase++) {
        phase_pressure_init_live(ctx, phase);
        assert_memory_footprint_within(ctx->live, ARRAY_LEN(ctx->live), 4,
                                       FOOTPRINT_SLACK_PAGES,
                                       "parallel_phase_pressure");

        rc = pthread_barrier_wait(ctx->barrier);
        tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
                  "pthread_barrier_wait should succeed");

        phase_pressure_rotate(ctx, phase);
        if (((phase + ctx->tid) & 1) == 0) {
            sched_yield();
        }

        rc = pthread_barrier_wait(ctx->barrier);
        tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
                  "pthread_barrier_wait should succeed");

        free_all_blocks(ctx->scratch, ARRAY_LEN(ctx->scratch));
        for (size_t i = 0; i < ARRAY_LEN(ctx->live); i++) {
            free_block(&ctx->live[i]);
        }

        rc = pthread_barrier_wait(ctx->barrier);
        tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
                  "pthread_barrier_wait should succeed");
    }

    return NULL;
}

StressSystemTest(parallel_phase_pressure, ((const char *[]){})) {
    enum { THREADS = 6 };

    pthread_barrier_t barrier;
    pthread_t threads[THREADS];
    phase_pressure_thread_ctx_t ctx[THREADS];

    tk_assert(pthread_barrier_init(&barrier, NULL, THREADS) == 0,
              "pthread_barrier_init should succeed");

    for (int i = 0; i < THREADS; i++) {
        ctx[i] = (phase_pressure_thread_ctx_t){
            .barrier = &barrier,
            .tid = i,
        };
        tk_assert(pthread_create(&threads[i], NULL, thread_phase_pressure,
                                 &ctx[i]) == 0,
                  "pthread_create should succeed");
    }

    for (int i = 0; i < THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0,
                  "pthread_join should succeed");
    }
    tk_assert(pthread_barrier_destroy(&barrier) == 0,
              "pthread_barrier_destroy should succeed");
}

enum {
    SHARED_POOL_THREADS = 6,
    SHARED_POOL_SLOTS = 24,
    SHARED_POOL_ROUNDS = 64,
};

typedef struct {
    pthread_barrier_t *barrier;
    tracked_block_t pool[SHARED_POOL_SLOTS];
    tracked_block_t local[SHARED_POOL_SLOTS];
    int tid;
} shared_pool_thread_ctx_t;

static void init_shared_pool(shared_pool_thread_ctx_t *ctx) {
    for (size_t i = 0; i < SHARED_POOL_SLOTS; i++) {
        size_t size = shared_pool_size(0, i, ctx->tid);
        ctx->pool[i] = alloc_block(size, shared_pool_seed(0, i, ctx->tid),
                                   "parallel_shared_pool_reuse");
        clear_block(&ctx->local[i]);
    }
}

static void rotate_shared_pool(shared_pool_thread_ctx_t *ctx, size_t round) {
    for (size_t i = 0; i < SHARED_POOL_SLOTS; i++) {
        if ((i + round + (size_t)ctx->tid) % 3u == 0) {
            free_block(&ctx->pool[i]);
            ctx->pool[i] = alloc_block(
                shared_pool_size(round + 1u, i, ctx->tid),
                shared_pool_seed(round + 1u, i, ctx->tid),
                "parallel_shared_pool_reuse");
        }
    }

    for (size_t i = 0; i < SHARED_POOL_SLOTS; i++) {
        if ((i + round) % 4u == 0) {
            free_block(&ctx->local[i]);
            ctx->local[i] = alloc_block(
                64u + ((round + i) % 5u) * 23u,
                (unsigned char)(0x90u + ctx->tid * 9u + round + i),
                "parallel_shared_pool_reuse");
        }
    }

    assert_live_blocks_healthy(ctx->pool, SHARED_POOL_SLOTS,
                               "parallel_shared_pool_reuse");
    assert_live_blocks_healthy(ctx->local, SHARED_POOL_SLOTS,
                               "parallel_shared_pool_reuse");
}

static void *thread_shared_pool_reuse(void *arg) {
    shared_pool_thread_ctx_t *ctx = arg;

    init_shared_pool(ctx);
    barrier_wait_ok(ctx->barrier, "parallel_shared_pool_reuse");

    for (size_t round = 0; round < SHARED_POOL_ROUNDS; round++) {
        rotate_shared_pool(ctx, round);
        barrier_wait_ok(ctx->barrier, "parallel_shared_pool_reuse");

        for (size_t i = 0; i < SHARED_POOL_SLOTS; i++) {
            if (ctx->local[i].ptr != NULL && ((round + i) & 1u) == 0) {
                assert_pattern(&ctx->local[i], "parallel_shared_pool_reuse");
            }
        }

        barrier_wait_ok(ctx->barrier, "parallel_shared_pool_reuse");

        for (size_t i = 0; i < SHARED_POOL_SLOTS; i++) {
            if ((round + i + (size_t)ctx->tid) % 5u == 0) {
                free_block(&ctx->local[i]);
            }
        }

        barrier_wait_ok(ctx->barrier, "parallel_shared_pool_reuse");
    }

    return NULL;
}

StressSystemTest(parallel_shared_pool_reuse, ((const char *[]){})) {
    pthread_barrier_t barrier;
    pthread_t threads[SHARED_POOL_THREADS];
    shared_pool_thread_ctx_t ctx[SHARED_POOL_THREADS];

    tk_assert(pthread_barrier_init(&barrier, NULL, SHARED_POOL_THREADS) == 0,
              "pthread_barrier_init should succeed");

    for (int i = 0; i < SHARED_POOL_THREADS; i++) {
        ctx[i] = (shared_pool_thread_ctx_t){
            .barrier = &barrier,
            .tid = i,
        };
        tk_assert(pthread_create(&threads[i], NULL, thread_shared_pool_reuse,
                                 &ctx[i]) == 0,
                  "pthread_create should succeed");
    }

    for (int i = 0; i < SHARED_POOL_THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0,
                  "pthread_join should succeed");
    }
    tk_assert(pthread_barrier_destroy(&barrier) == 0,
              "pthread_barrier_destroy should succeed");

    for (int i = 0; i < SHARED_POOL_THREADS; i++) {
        free_all_blocks(ctx[i].local, SHARED_POOL_SLOTS);
        free_all_blocks(ctx[i].pool, SHARED_POOL_SLOTS);
    }
}

SystemTest(fragmented_big_rebuild, ((const char *[]){})) {
    enum {
        BLOCK_COUNT = 72,
    };

    tracked_block_t blocks[BLOCK_COUNT];

    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        size_t size = 128u + (i % 11u) * 29u;
        blocks[i] = alloc_block(size, (unsigned char)(0x40u + i),
                                "fragmented_big_rebuild");
    }
    assert_live_blocks_healthy(blocks, ARRAY_LEN(blocks),
                               "fragmented_big_rebuild");

    for (size_t i = 0; i < BLOCK_COUNT; i += 4) {
        free_block(&blocks[i]);
    }
    for (size_t i = 1; i < BLOCK_COUNT; i += 4) {
        free_block(&blocks[i]);
    }

    for (size_t i = 0; i < BLOCK_COUNT; i += 4) {
        blocks[i] = alloc_block(96u + (i % 5u) * 24u,
                                (unsigned char)(0x70u + i),
                                "fragmented_big_rebuild");
    }
    for (size_t i = 1; i < BLOCK_COUNT; i += 4) {
        blocks[i] = alloc_block(112u + (i % 7u) * 18u,
                                (unsigned char)(0x90u + i),
                                "fragmented_big_rebuild");
    }

    assert_live_blocks_healthy(blocks, ARRAY_LEN(blocks),
                               "fragmented_big_rebuild");

    for (size_t i = 2; i < BLOCK_COUNT; i += 2) {
        if (blocks[i].ptr != NULL) {
            free_block(&blocks[i]);
        }
    }

    tracked_block_t big = alloc_block(8192u, 0xF4, "fragmented_big_rebuild");
    assert_pattern(&blocks[0], "fragmented_big_rebuild");
    assert_pattern(&blocks[1], "fragmented_big_rebuild");

    free_block(&big);
    free_all_blocks(blocks, ARRAY_LEN(blocks));
}

enum {
    TRANSFER_THREADS = 4,
    TRANSFER_COUNT = 48,
};

typedef struct {
    pthread_barrier_t barrier;
    tracked_block_t owned[TRANSFER_COUNT];
    tracked_block_t recycled[TRANSFER_COUNT / 2];
} cross_thread_transfer_shared_t;

typedef struct {
    cross_thread_transfer_shared_t *shared;
    int tid;
} cross_thread_transfer_ctx_t;

static size_t transfer_block_size(size_t index) {
    static const size_t sizes[] = {
        24,   31,   32,   33,   63,   64,   65,   127,
        128,  129,  255,  256,  257,  511,  512,  513,
        1023, 1024, 1025, 2047, 2048, 2049,
    };

    return sizes[index % ARRAY_LEN(sizes)] + (index / ARRAY_LEN(sizes)) * 8u;
}

static void *thread_cross_thread_transfer(void *arg) {
    cross_thread_transfer_ctx_t *ctx = arg;
    cross_thread_transfer_shared_t *shared = ctx->shared;

    if (ctx->tid == 0) {
        for (size_t i = 0; i < TRANSFER_COUNT; i++) {
            shared->owned[i] = alloc_block(
                transfer_block_size(i),
                (unsigned char)(0x20u + i),
                "cross_thread_free_and_reuse");
        }
    }
    barrier_wait_ok(&shared->barrier, "cross_thread_free_and_reuse");

    if (ctx->tid == 1) {
        for (size_t i = 0; i < TRANSFER_COUNT; i += 2) {
            free_block(&shared->owned[i]);
        }
    }
    barrier_wait_ok(&shared->barrier, "cross_thread_free_and_reuse");

    if (ctx->tid == 2) {
        for (size_t i = 0; i < TRANSFER_COUNT / 2; i++) {
            size_t source = i * 2u;
            shared->recycled[i] = alloc_block(
                transfer_block_size(source),
                (unsigned char)(0x80u + i),
                "cross_thread_free_and_reuse");
        }
    }
    barrier_wait_ok(&shared->barrier, "cross_thread_free_and_reuse");

    if (ctx->tid == 3) {
        tracked_block_t live[TRANSFER_COUNT + TRANSFER_COUNT / 2];
        for (size_t i = 0; i < TRANSFER_COUNT; i++) {
            live[i] = shared->owned[i];
        }
        for (size_t i = 0; i < TRANSFER_COUNT / 2; i++) {
            live[TRANSFER_COUNT + i] = shared->recycled[i];
        }
        assert_live_blocks_healthy(live, ARRAY_LEN(live),
                                   "cross_thread_free_and_reuse");
        assert_memory_footprint_within(live, ARRAY_LEN(live), 4,
                                       FOOTPRINT_SLACK_PAGES,
                                       "cross_thread_free_and_reuse");
    }
    barrier_wait_ok(&shared->barrier, "cross_thread_free_and_reuse");

    if (ctx->tid == 0) {
        for (size_t i = 1; i < TRANSFER_COUNT; i += 2) {
            free_block(&shared->owned[i]);
        }
    } else if (ctx->tid == 2) {
        free_all_blocks(shared->recycled, ARRAY_LEN(shared->recycled));
    }
    barrier_wait_ok(&shared->barrier, "cross_thread_free_and_reuse");

    return NULL;
}

StressSystemTest(cross_thread_free_and_reuse, ((const char *[]){})) {
    pthread_t threads[TRANSFER_THREADS];
    cross_thread_transfer_ctx_t ctx[TRANSFER_THREADS];
    cross_thread_transfer_shared_t shared = {0};

    tk_assert(pthread_barrier_init(&shared.barrier, NULL, TRANSFER_THREADS) == 0,
              "pthread_barrier_init should succeed");

    for (int i = 0; i < TRANSFER_THREADS; i++) {
        ctx[i] = (cross_thread_transfer_ctx_t){
            .shared = &shared,
            .tid = i,
        };
        tk_assert(pthread_create(&threads[i], NULL, thread_cross_thread_transfer,
                                 &ctx[i]) == 0,
                  "pthread_create should succeed");
    }

    for (int i = 0; i < TRANSFER_THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0,
                  "pthread_join should succeed");
    }
    tk_assert(pthread_barrier_destroy(&shared.barrier) == 0,
              "pthread_barrier_destroy should succeed");
}

enum {
    THREAD_COUNT = 4,
    BLOCKS_PER_THREAD = 32,
};

typedef struct {
    pthread_barrier_t *barrier;
    tracked_block_t blocks[BLOCKS_PER_THREAD];
    int tid;
} live_alloc_thread_ctx_t;

static void *thread_alloc_blocks(void *arg) {
    live_alloc_thread_ctx_t *ctx = arg;
    int rc = pthread_barrier_wait(ctx->barrier);
    tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
              "pthread_barrier_wait should succeed");

    for (size_t i = 0; i < BLOCKS_PER_THREAD; i++) {
        size_t size = 24u + (size_t)ctx->tid * 8u + (i % 7u) * 9u;
        ctx->blocks[i] = alloc_block(
            size, (unsigned char)(0xA0u + ctx->tid * 13u + i),
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
    live_alloc_thread_ctx_t ctx[THREAD_COUNT];
    tracked_block_t all_blocks[THREAD_COUNT * BLOCKS_PER_THREAD];

    tk_assert(pthread_barrier_init(&barrier, NULL, THREAD_COUNT + 1) == 0,
              "pthread_barrier_init should succeed");

    for (int i = 0; i < THREAD_COUNT; i++) {
        ctx[i] = (live_alloc_thread_ctx_t){
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

    assert_live_blocks_healthy(all_blocks, ARRAY_LEN(all_blocks),
                               "concurrent_live_allocations");

    free_all_blocks(all_blocks, ARRAY_LEN(all_blocks));
}

enum {
    CHURN_THREADS = 4,
    CHURN_SLOTS_PER_THREAD = 8,
    CHURN_ROUNDS = 96,
};

typedef struct {
    pthread_barrier_t *barrier;
    tracked_block_t slots[CHURN_SLOTS_PER_THREAD];
    uint32_t state;
    int tid;
} churn_thread_ctx_t;

static void *thread_round_churn(void *arg) {
    churn_thread_ctx_t *ctx = arg;

    for (size_t round = 0; round < CHURN_ROUNDS; round++) {
        int rc = pthread_barrier_wait(ctx->barrier);
        tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
                  "pthread_barrier_wait should succeed");

        uint32_t action = next_u32(&ctx->state);
        size_t slot = action % CHURN_SLOTS_PER_THREAD;

        if (ctx->slots[slot].ptr != NULL && (action & 1u) == 0) {
            assert_pattern(&ctx->slots[slot],
                           "concurrent_round_based_churn");
            free_block(&ctx->slots[slot]);
        } else {
            if (ctx->slots[slot].ptr != NULL) {
                assert_pattern(&ctx->slots[slot],
                               "concurrent_round_based_churn");
                free_block(&ctx->slots[slot]);
            }

            size_t size = select_small_size(next_u32(&ctx->state)) +
                          (size_t)ctx->tid * 3u;
            ctx->slots[slot] = alloc_block(
                size,
                (unsigned char)(0xC0u + ctx->tid * 19u + round + slot),
                "concurrent_round_based_churn");
        }

        assert_live_blocks_healthy(ctx->slots, CHURN_SLOTS_PER_THREAD,
                                   "concurrent_round_based_churn");
        if (((round + (size_t)ctx->tid) & 3u) == 0) {
            sched_yield();
        }

        rc = pthread_barrier_wait(ctx->barrier);
        tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
                  "pthread_barrier_wait should succeed");
    }

    return NULL;
}

static void assert_churn_threads_healthy(const churn_thread_ctx_t *ctx) {
    tracked_block_t all_blocks[CHURN_THREADS * CHURN_SLOTS_PER_THREAD];

    for (size_t i = 0; i < CHURN_THREADS; i++) {
        for (size_t j = 0; j < CHURN_SLOTS_PER_THREAD; j++) {
            all_blocks[i * CHURN_SLOTS_PER_THREAD + j] = ctx[i].slots[j];
        }
    }

    assert_live_blocks_healthy(all_blocks, ARRAY_LEN(all_blocks),
                               "concurrent_round_based_churn");
    assert_memory_footprint_within(all_blocks, ARRAY_LEN(all_blocks), 4,
                                   FOOTPRINT_SLACK_PAGES + 4u,
                                   "concurrent_round_based_churn");
}

StressSystemTest(concurrent_round_based_churn, ((const char *[]){})) {
    pthread_barrier_t barrier;
    pthread_t threads[CHURN_THREADS];
    churn_thread_ctx_t ctx[CHURN_THREADS];

    tk_assert(pthread_barrier_init(&barrier, NULL, CHURN_THREADS + 1) == 0,
              "pthread_barrier_init should succeed");

    for (int i = 0; i < CHURN_THREADS; i++) {
        ctx[i] = (churn_thread_ctx_t){
            .barrier = &barrier,
            .state = 0x12340000u + (uint32_t)i * 0x101u,
            .tid = i,
        };
        tk_assert(pthread_create(&threads[i], NULL, thread_round_churn,
                                 &ctx[i]) == 0,
                  "pthread_create should succeed");
    }

    for (size_t round = 0; round < CHURN_ROUNDS; round++) {
        int rc = pthread_barrier_wait(&barrier);
        tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
                  "pthread_barrier_wait should succeed");
        rc = pthread_barrier_wait(&barrier);
        tk_assert(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD,
                  "pthread_barrier_wait should succeed");
        assert_churn_threads_healthy(ctx);
    }

    for (int i = 0; i < CHURN_THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0,
                  "pthread_join should succeed");
    }
    tk_assert(pthread_barrier_destroy(&barrier) == 0,
              "pthread_barrier_destroy should succeed");

    for (size_t i = 0; i < CHURN_THREADS; i++) {
        free_all_blocks(ctx[i].slots, CHURN_SLOTS_PER_THREAD);
    }
}
