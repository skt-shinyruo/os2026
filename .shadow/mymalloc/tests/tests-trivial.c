// Local tests for M5 mymalloc/myfree.
//
// These tests intentionally exercise allocator invariants instead of
// assuming a specific implementation strategy.

#include <mymalloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <testkit.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9e3779b9u;
    return *state;
}

static int is_aligned_8(const void *ptr) {
    return ((uintptr_t)ptr & 7u) == 0;
}

static uint8_t pattern_byte(uint32_t tag, size_t offset) {
    return (uint8_t)(tag ^ (uint32_t)(offset * 131u) ^ (uint32_t)(offset >> 7));
}

static void fill_block(void *ptr, size_t size, uint32_t tag) {
    uint8_t *p = ptr;
    for (size_t i = 0; i < size; i++) {
        p[i] = pattern_byte(tag, i);
    }
}

static void check_block(void *ptr, size_t size, uint32_t tag) {
    uint8_t *p = ptr;
    for (size_t i = 0; i < size; i++) {
        tk_assert(p[i] == pattern_byte(tag, i),
                  "block corrupted at byte %zu: got %u, expected %u",
                  i, p[i], pattern_byte(tag, i));
    }
}

static int ranges_overlap(void *a, size_t asize, void *b, size_t bsize) {
    uintptr_t lo_a = (uintptr_t)a;
    uintptr_t hi_a = lo_a + asize;
    uintptr_t lo_b = (uintptr_t)b;
    uintptr_t hi_b = lo_b + bsize;
    return lo_a < hi_b && lo_b < hi_a;
}

struct alloc_rec {
    void *ptr;
    size_t size;
    uint32_t tag;
};

static void check_no_overlap_with(struct alloc_rec *recs, size_t n,
                                  void *ptr, size_t size) {
    for (size_t i = 0; i < n; i++) {
        if (!recs[i].ptr) {
            continue;
        }
        tk_assert(!ranges_overlap(recs[i].ptr, recs[i].size, ptr, size),
                  "active ranges overlap: old=%p+%zu new=%p+%zu",
                  recs[i].ptr, recs[i].size, ptr, size);
    }
}

static void assert_alloc_ok(void *ptr, size_t size) {
    tk_assert(ptr != NULL, "mymalloc(%zu) returned NULL", size);
    tk_assert(is_aligned_8(ptr), "mymalloc(%zu) returned unaligned %p",
              size, ptr);
}

SystemTest(vmalloc_page_alignment, ((const char *[]){ })) {
    void *p1 = vmalloc(NULL, 4096);
    tk_assert(p1 != NULL, "vmalloc(4096) should not return NULL");
    tk_assert((uintptr_t)p1 % 4096 == 0,
              "vmalloc should return a page-aligned address");

    void *p2 = vmalloc(NULL, 8192);
    tk_assert(p2 != NULL, "vmalloc(8192) should not return NULL");
    tk_assert((uintptr_t)p2 % 4096 == 0,
              "vmalloc should return a page-aligned address");
    tk_assert(p1 != p2, "different vmalloc calls should not overlap exactly");

    vmfree(p1, 4096);
    vmfree(p2, 8192);
}

UnitTest(zero_size_requests_do_not_deadlock) {
    for (int i = 0; i < 10000; i++) {
        void *ptr = mymalloc(0);
        if (ptr) {
            tk_assert(is_aligned_8(ptr), "mymalloc(0) returned unaligned %p",
                      ptr);
            myfree(ptr);
        }
    }
}

UnitTest(alignment_and_access_for_many_sizes) {
    static const size_t sizes[] = {
        1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 31, 32, 33,
        63, 64, 65, 127, 128, 129, 255, 256, 257,
        511, 512, 513, 1023, 1024, 4095, 4096, 4097
    };
    struct alloc_rec recs[ARRAY_LEN(sizes)] = { 0 };

    for (size_t i = 0; i < ARRAY_LEN(sizes); i++) {
        void *ptr = mymalloc(sizes[i]);
        assert_alloc_ok(ptr, sizes[i]);
        check_no_overlap_with(recs, i, ptr, sizes[i]);
        recs[i] = (struct alloc_rec) {
            .ptr = ptr,
            .size = sizes[i],
            .tag = 0x1000u + (uint32_t)i,
        };
        fill_block(ptr, sizes[i], recs[i].tag);
    }

    for (size_t i = 0; i < ARRAY_LEN(sizes); i++) {
        check_block(recs[i].ptr, recs[i].size, recs[i].tag);
        myfree(recs[i].ptr);
    }
}

UnitTest(large_allocations_and_page_boundaries) {
    static const size_t sizes[] = {
        4095,
        4096,
        4097,
        64 * 1024,
        1024 * 1024,
        4 * 1024 * 1024 + 1,
    };
    struct alloc_rec recs[ARRAY_LEN(sizes)] = { 0 };

    for (size_t i = 0; i < ARRAY_LEN(sizes); i++) {
        void *ptr = mymalloc(sizes[i]);
        assert_alloc_ok(ptr, sizes[i]);
        check_no_overlap_with(recs, i, ptr, sizes[i]);
        recs[i] = (struct alloc_rec) {
            .ptr = ptr,
            .size = sizes[i],
            .tag = 0x180000u + (uint32_t)i,
        };
        fill_block(ptr, sizes[i], recs[i].tag);
    }

    for (size_t i = 0; i < ARRAY_LEN(sizes); i++) {
        check_block(recs[i].ptr, recs[i].size, recs[i].tag);
        myfree(recs[i].ptr);
    }
}

UnitTest(active_allocations_do_not_overlap) {
    enum { N = 512 };
    struct alloc_rec recs[N] = { 0 };

    for (int i = 0; i < N; i++) {
        size_t size = (size_t)((i * 37) % 2048) + 1;
        void *ptr = mymalloc(size);
        assert_alloc_ok(ptr, size);
        check_no_overlap_with(recs, (size_t)i, ptr, size);
        recs[i] = (struct alloc_rec) {
            .ptr = ptr,
            .size = size,
            .tag = 0x200000u + (uint32_t)i,
        };
        fill_block(ptr, size, recs[i].tag);
    }

    for (int i = 0; i < N; i++) {
        check_block(recs[i].ptr, recs[i].size, recs[i].tag);
        myfree(recs[i].ptr);
    }
}

UnitTest(sequential_random_model) {
    enum { SLOTS = 256, OPS = 12000 };
    struct alloc_rec slots[SLOTS] = { 0 };
    uint32_t rng = 0x12345678u;

    for (int op = 0; op < OPS; op++) {
        size_t slot = rng_next(&rng) % SLOTS;
        if (slots[slot].ptr && (rng_next(&rng) & 1u)) {
            check_block(slots[slot].ptr, slots[slot].size, slots[slot].tag);
            myfree(slots[slot].ptr);
            slots[slot] = (struct alloc_rec){ 0 };
            continue;
        }

        if (!slots[slot].ptr) {
            size_t size = (rng_next(&rng) % 512u) + 1u;
            void *ptr = mymalloc(size);
            assert_alloc_ok(ptr, size);
            check_no_overlap_with(slots, SLOTS, ptr, size);
            slots[slot] = (struct alloc_rec) {
                .ptr = ptr,
                .size = size,
                .tag = 0x300000u + (uint32_t)op,
            };
            fill_block(ptr, size, slots[slot].tag);
        }
    }

    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].ptr) {
            check_block(slots[i].ptr, slots[i].size, slots[i].tag);
            myfree(slots[i].ptr);
        }
    }
}

#define LIVE_CAPACITY 32768

static pthread_mutex_t live_lock = PTHREAD_MUTEX_INITIALIZER;
static struct alloc_rec live_ranges[LIVE_CAPACITY];
static size_t live_count;

static void live_reset(void) {
    pthread_mutex_lock(&live_lock);
    live_count = 0;
    pthread_mutex_unlock(&live_lock);
}

static void live_add(void *ptr, size_t size, uint32_t tag) {
    pthread_mutex_lock(&live_lock);
    tk_assert(live_count < LIVE_CAPACITY, "live range table is full");
    check_no_overlap_with(live_ranges, live_count, ptr, size);
    live_ranges[live_count++] = (struct alloc_rec) {
        .ptr = ptr,
        .size = size,
        .tag = tag,
    };
    pthread_mutex_unlock(&live_lock);
}

static void live_remove(void *ptr) {
    int found = 0;

    pthread_mutex_lock(&live_lock);
    for (size_t i = 0; i < live_count; i++) {
        if (live_ranges[i].ptr == ptr) {
            live_ranges[i] = live_ranges[--live_count];
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&live_lock);

    tk_assert(found, "attempted to remove unknown live pointer %p", ptr);
}

struct worker_arg {
    int id;
    uint32_t seed;
    atomic_int *go;
};

static void *alloc_free_worker(void *arg_ptr) {
    enum { LOCAL_SLOTS = 64, OPS = 3000 };
    struct worker_arg *arg = arg_ptr;
    struct alloc_rec slots[LOCAL_SLOTS] = { 0 };
    uint32_t rng = arg->seed;

    while (!atomic_load_explicit(arg->go, memory_order_acquire)) {
    }

    for (int op = 0; op < OPS; op++) {
        size_t slot = rng_next(&rng) % LOCAL_SLOTS;
        if (slots[slot].ptr && (rng_next(&rng) % 3u != 0u)) {
            check_block(slots[slot].ptr, slots[slot].size, slots[slot].tag);
            live_remove(slots[slot].ptr);
            myfree(slots[slot].ptr);
            slots[slot] = (struct alloc_rec){ 0 };
        } else if (!slots[slot].ptr) {
            size_t size = (rng_next(&rng) % 384u) + 1u;
            uint32_t tag = 0x400000u + (uint32_t)arg->id * 100000u
                         + (uint32_t)op;
            void *ptr = mymalloc(size);
            assert_alloc_ok(ptr, size);
            live_add(ptr, size, tag);
            slots[slot] = (struct alloc_rec) {
                .ptr = ptr,
                .size = size,
                .tag = tag,
            };
            fill_block(ptr, size, tag);
        }
    }

    for (int i = 0; i < LOCAL_SLOTS; i++) {
        if (slots[i].ptr) {
            check_block(slots[i].ptr, slots[i].size, slots[i].tag);
            live_remove(slots[i].ptr);
            myfree(slots[i].ptr);
        }
    }

    return NULL;
}

UnitTest(concurrent_alloc_free_workload) {
    enum { THREADS = 6 };
    pthread_t threads[THREADS];
    struct worker_arg args[THREADS];
    atomic_int go;

    live_reset();
    atomic_init(&go, 0);

    for (int i = 0; i < THREADS; i++) {
        args[i] = (struct worker_arg) {
            .id = i,
            .seed = 0xa5a50000u + (uint32_t)i * 977u,
            .go = &go,
        };
        tk_assert(pthread_create(&threads[i], NULL, alloc_free_worker,
                                 &args[i]) == 0,
                  "pthread_create failed");
    }

    atomic_store_explicit(&go, 1, memory_order_release);

    for (int i = 0; i < THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0, "pthread_join failed");
    }

    tk_assert(live_count == 0, "concurrent test leaked %zu live ranges",
              live_count);
}

struct hold_worker_arg {
    int id;
    atomic_int *go;
    void **ptrs;
    size_t *sizes;
    uint32_t *tags;
    int count;
};

static void *hold_alloc_worker(void *arg_ptr) {
    struct hold_worker_arg *arg = arg_ptr;

    while (!atomic_load_explicit(arg->go, memory_order_acquire)) {
    }

    for (int i = 0; i < arg->count; i++) {
        size_t size = (size_t)(((arg->id + 1) * 23 + i * 41) % 1024) + 1u;
        uint32_t tag = 0x480000u + (uint32_t)arg->id * 10000u
                     + (uint32_t)i;
        void *ptr = mymalloc(size);
        assert_alloc_ok(ptr, size);
        live_add(ptr, size, tag);
        fill_block(ptr, size, tag);
        arg->ptrs[i] = ptr;
        arg->sizes[i] = size;
        arg->tags[i] = tag;
    }

    return NULL;
}

UnitTest(concurrent_live_allocations_do_not_overlap) {
    enum { THREADS = 8, PER_THREAD = 384 };
    pthread_t threads[THREADS];
    struct hold_worker_arg args[THREADS];
    static void *ptrs[THREADS][PER_THREAD];
    static size_t sizes[THREADS][PER_THREAD];
    static uint32_t tags[THREADS][PER_THREAD];
    atomic_int go;

    live_reset();
    atomic_init(&go, 0);

    for (int i = 0; i < THREADS; i++) {
        args[i] = (struct hold_worker_arg) {
            .id = i,
            .go = &go,
            .ptrs = ptrs[i],
            .sizes = sizes[i],
            .tags = tags[i],
            .count = PER_THREAD,
        };
        tk_assert(pthread_create(&threads[i], NULL, hold_alloc_worker,
                                 &args[i]) == 0,
                  "pthread_create failed");
    }

    atomic_store_explicit(&go, 1, memory_order_release);

    for (int i = 0; i < THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0, "pthread_join failed");
    }

    tk_assert(live_count == THREADS * PER_THREAD,
              "expected %d live ranges, got %zu",
              THREADS * PER_THREAD, live_count);

    for (int t = 0; t < THREADS; t++) {
        for (int i = 0; i < PER_THREAD; i++) {
            check_block(ptrs[t][i], sizes[t][i], tags[t][i]);
            live_remove(ptrs[t][i]);
            myfree(ptrs[t][i]);
        }
    }

    tk_assert(live_count == 0, "live range table was not drained");
}

struct burst_worker_arg {
    int id;
    atomic_int *go;
    void **ptrs;
    int count;
    size_t size;
};

static void *burst_alloc_worker(void *arg_ptr) {
    struct burst_worker_arg *arg = arg_ptr;

    while (!atomic_load_explicit(arg->go, memory_order_acquire)) {
    }

    for (int i = 0; i < arg->count; i++) {
        uint32_t tag = 0x490000u + (uint32_t)arg->id * 10000u
                     + (uint32_t)i;
        void *ptr = mymalloc(arg->size);
        assert_alloc_ok(ptr, arg->size);
        live_add(ptr, arg->size, tag);
        fill_block(ptr, arg->size, tag);
        arg->ptrs[i] = ptr;
    }

    return NULL;
}

UnitTest(concurrent_burst_same_size_allocations) {
    enum { THREADS = 12, PER_THREAD = 256, SIZE = 64 };
    pthread_t threads[THREADS];
    struct burst_worker_arg args[THREADS];
    static void *ptrs[THREADS][PER_THREAD];
    atomic_int go;

    live_reset();
    atomic_init(&go, 0);

    for (int i = 0; i < THREADS; i++) {
        args[i] = (struct burst_worker_arg) {
            .id = i,
            .go = &go,
            .ptrs = ptrs[i],
            .count = PER_THREAD,
            .size = SIZE,
        };
        tk_assert(pthread_create(&threads[i], NULL, burst_alloc_worker,
                                 &args[i]) == 0,
                  "pthread_create failed");
    }

    atomic_store_explicit(&go, 1, memory_order_release);

    for (int i = 0; i < THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0, "pthread_join failed");
    }

    tk_assert(live_count == THREADS * PER_THREAD,
              "expected %d live ranges, got %zu",
              THREADS * PER_THREAD, live_count);

    for (int t = 0; t < THREADS; t++) {
        for (int i = 0; i < PER_THREAD; i++) {
            uint32_t tag = 0x490000u + (uint32_t)t * 10000u
                         + (uint32_t)i;
            check_block(ptrs[t][i], SIZE, tag);
            live_remove(ptrs[t][i]);
            myfree(ptrs[t][i]);
        }
    }

    tk_assert(live_count == 0, "live range table was not drained");
}

#define PIPELINE_N 4096
#define PIPELINE_PRODUCERS 4
#define PIPELINE_CONSUMERS 4

struct pipeline_slot {
    _Atomic(uintptr_t) ptr;
    size_t size;
    uint32_t tag;
};

static struct pipeline_slot pipeline_slots[PIPELINE_N];
static atomic_int pipeline_go;

static void *pipeline_producer(void *arg_ptr) {
    int id = (int)(intptr_t)arg_ptr;

    while (!atomic_load_explicit(&pipeline_go, memory_order_acquire)) {
    }

    for (int i = id; i < PIPELINE_N; i += PIPELINE_PRODUCERS) {
        size_t size = (size_t)((i * 29 + id * 17) % 768) + 1u;
        uint32_t tag = 0x4a0000u + (uint32_t)i;
        void *ptr = mymalloc(size);
        assert_alloc_ok(ptr, size);

        live_add(ptr, size, tag);
        fill_block(ptr, size, tag);
        pipeline_slots[i].size = size;
        pipeline_slots[i].tag = tag;
        atomic_store_explicit(&pipeline_slots[i].ptr, (uintptr_t)ptr,
                              memory_order_release);
    }

    return NULL;
}

static void *pipeline_consumer(void *arg_ptr) {
    int id = (int)(intptr_t)arg_ptr;

    while (!atomic_load_explicit(&pipeline_go, memory_order_acquire)) {
    }

    for (int i = id; i < PIPELINE_N; i += PIPELINE_CONSUMERS) {
        uintptr_t raw;
        do {
            raw = atomic_load_explicit(&pipeline_slots[i].ptr,
                                       memory_order_acquire);
        } while (raw == 0);

        void *ptr = (void *)raw;
        check_block(ptr, pipeline_slots[i].size, pipeline_slots[i].tag);
        live_remove(ptr);
        myfree(ptr);
        atomic_store_explicit(&pipeline_slots[i].ptr, 0,
                              memory_order_release);
    }

    return NULL;
}

UnitTest(concurrent_producer_consumer_cross_thread_free) {
    pthread_t producers[PIPELINE_PRODUCERS];
    pthread_t consumers[PIPELINE_CONSUMERS];

    live_reset();
    for (int i = 0; i < PIPELINE_N; i++) {
        atomic_store_explicit(&pipeline_slots[i].ptr, 0,
                              memory_order_relaxed);
        pipeline_slots[i].size = 0;
        pipeline_slots[i].tag = 0;
    }

    atomic_store_explicit(&pipeline_go, 0, memory_order_relaxed);

    for (int i = 0; i < PIPELINE_PRODUCERS; i++) {
        tk_assert(pthread_create(&producers[i], NULL, pipeline_producer,
                                 (void *)(intptr_t)i) == 0,
                  "pthread_create producer failed");
    }

    for (int i = 0; i < PIPELINE_CONSUMERS; i++) {
        tk_assert(pthread_create(&consumers[i], NULL, pipeline_consumer,
                                 (void *)(intptr_t)i) == 0,
                  "pthread_create consumer failed");
    }

    atomic_store_explicit(&pipeline_go, 1, memory_order_release);

    for (int i = 0; i < PIPELINE_PRODUCERS; i++) {
        tk_assert(pthread_join(producers[i], NULL) == 0,
                  "pthread_join producer failed");
    }
    for (int i = 0; i < PIPELINE_CONSUMERS; i++) {
        tk_assert(pthread_join(consumers[i], NULL) == 0,
                  "pthread_join consumer failed");
    }

    tk_assert(live_count == 0, "pipeline test leaked %zu live ranges",
              live_count);
}

#define REMOTE_STORM_N 4096
#define REMOTE_STORM_ALLOCATORS 4
#define REMOTE_STORM_FREERS 6

struct remote_storm_slot {
    _Atomic(uintptr_t) ptr;
    size_t size;
    uint32_t tag;
};

static struct remote_storm_slot remote_storm_slots[REMOTE_STORM_N];
static atomic_int remote_storm_go;
static atomic_int remote_storm_next_free;

static void *remote_storm_allocator(void *arg_ptr) {
    int id = (int)(intptr_t)arg_ptr;
    uint32_t rng = 0x4c0000u + (uint32_t)id * 977u;

    while (!atomic_load_explicit(&remote_storm_go, memory_order_acquire)) {
    }

    for (int i = id; i < REMOTE_STORM_N; i += REMOTE_STORM_ALLOCATORS) {
        size_t size = (rng_next(&rng) % 1024u) + 1u;
        uint32_t tag = 0x4d0000u + (uint32_t)i;
        void *ptr = mymalloc(size);
        assert_alloc_ok(ptr, size);
        live_add(ptr, size, tag);
        fill_block(ptr, size, tag);
        remote_storm_slots[i].size = size;
        remote_storm_slots[i].tag = tag;
        atomic_store_explicit(&remote_storm_slots[i].ptr, (uintptr_t)ptr,
                              memory_order_release);
    }

    return NULL;
}

static void *remote_storm_freer(void *arg_ptr) {
    (void)arg_ptr;

    while (!atomic_load_explicit(&remote_storm_go, memory_order_acquire)) {
    }

    for (;;) {
        int i = atomic_fetch_add_explicit(&remote_storm_next_free, 1,
                                          memory_order_relaxed);
        if (i >= REMOTE_STORM_N) {
            break;
        }

        uintptr_t raw;
        do {
            raw = atomic_load_explicit(&remote_storm_slots[i].ptr,
                                       memory_order_acquire);
        } while (raw == 0);

        void *ptr = (void *)raw;
        check_block(ptr, remote_storm_slots[i].size,
                    remote_storm_slots[i].tag);
        live_remove(ptr);
        myfree(ptr);
        atomic_store_explicit(&remote_storm_slots[i].ptr, 0,
                              memory_order_release);
    }

    return NULL;
}

UnitTest(remote_free_storm_while_allocators_run) {
    pthread_t allocators[REMOTE_STORM_ALLOCATORS];
    pthread_t freers[REMOTE_STORM_FREERS];

    live_reset();
    for (int i = 0; i < REMOTE_STORM_N; i++) {
        atomic_store_explicit(&remote_storm_slots[i].ptr, 0,
                              memory_order_relaxed);
        remote_storm_slots[i].size = 0;
        remote_storm_slots[i].tag = 0;
    }
    atomic_store_explicit(&remote_storm_next_free, 0, memory_order_relaxed);
    atomic_store_explicit(&remote_storm_go, 0, memory_order_relaxed);

    for (int i = 0; i < REMOTE_STORM_ALLOCATORS; i++) {
        tk_assert(pthread_create(&allocators[i], NULL, remote_storm_allocator,
                                 (void *)(intptr_t)i) == 0,
                  "pthread_create allocator failed");
    }
    for (int i = 0; i < REMOTE_STORM_FREERS; i++) {
        tk_assert(pthread_create(&freers[i], NULL, remote_storm_freer,
                                 (void *)(intptr_t)i) == 0,
                  "pthread_create freer failed");
    }

    atomic_store_explicit(&remote_storm_go, 1, memory_order_release);

    for (int i = 0; i < REMOTE_STORM_ALLOCATORS; i++) {
        tk_assert(pthread_join(allocators[i], NULL) == 0,
                  "pthread_join allocator failed");
    }
    for (int i = 0; i < REMOTE_STORM_FREERS; i++) {
        tk_assert(pthread_join(freers[i], NULL) == 0,
                  "pthread_join freer failed");
    }

    tk_assert(live_count == 0, "remote free storm leaked %zu live ranges",
              live_count);
}

#define CROSS_THREAD_N 2048
#define CROSS_THREAD_THREADS 4

static struct alloc_rec cross_thread_blocks[CROSS_THREAD_N];
static atomic_int cross_thread_go;

static void *cross_thread_free_worker(void *arg_ptr) {
    int id = (int)(intptr_t)arg_ptr;

    while (!atomic_load_explicit(&cross_thread_go, memory_order_acquire)) {
    }

    for (int i = id; i < CROSS_THREAD_N; i += CROSS_THREAD_THREADS) {
        check_block(cross_thread_blocks[i].ptr,
                    cross_thread_blocks[i].size,
                    cross_thread_blocks[i].tag);
        myfree(cross_thread_blocks[i].ptr);
        cross_thread_blocks[i].ptr = NULL;
    }

    return NULL;
}

UnitTest(cross_thread_free_workload) {
    pthread_t threads[CROSS_THREAD_THREADS];

    for (int i = 0; i < CROSS_THREAD_N; i++) {
        size_t size = (size_t)((i * 19) % 512) + 1;
        void *ptr = mymalloc(size);
        assert_alloc_ok(ptr, size);
        check_no_overlap_with(cross_thread_blocks, (size_t)i, ptr, size);
        cross_thread_blocks[i] = (struct alloc_rec) {
            .ptr = ptr,
            .size = size,
            .tag = 0x500000u + (uint32_t)i,
        };
        fill_block(ptr, size, cross_thread_blocks[i].tag);
    }

    atomic_store_explicit(&cross_thread_go, 0, memory_order_relaxed);
    for (int i = 0; i < CROSS_THREAD_THREADS; i++) {
        tk_assert(pthread_create(&threads[i], NULL, cross_thread_free_worker,
                                 (void *)(intptr_t)i) == 0,
                  "pthread_create failed");
    }

    atomic_store_explicit(&cross_thread_go, 1, memory_order_release);

    for (int i = 0; i < CROSS_THREAD_THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0, "pthread_join failed");
    }

    for (int i = 0; i < CROSS_THREAD_N; i++) {
        tk_assert(cross_thread_blocks[i].ptr == NULL,
                  "block %d was not freed by worker threads", i);
    }
}

#ifdef LOCAL_PERF_SMOKE

static long long monotonic_ns(void) {
    struct timespec ts;
    tk_assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0,
              "clock_gettime failed");
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

struct perf_worker_arg {
    int id;
    atomic_int *go;
};

static void *perf_worker(void *arg_ptr) {
    enum { OPS = 25000 };
    struct perf_worker_arg *arg = arg_ptr;

    while (!atomic_load_explicit(arg->go, memory_order_acquire)) {
    }

    for (int i = 0; i < OPS; i++) {
        size_t size = (size_t)(((i + arg->id * 17) % 256) + 1);
        void *ptr = mymalloc(size);
        assert_alloc_ok(ptr, size);
        fill_block(ptr, size, 0x5a0000u + (uint32_t)arg->id * 100000u
                         + (uint32_t)i);
        check_block(ptr, size, 0x5a0000u + (uint32_t)arg->id * 100000u
                          + (uint32_t)i);
        myfree(ptr);
    }

    return NULL;
}

UnitTest(parallel_throughput_smoke) {
    enum { THREADS = 8 };
    pthread_t threads[THREADS];
    struct perf_worker_arg args[THREADS];
    atomic_int go;

    atomic_init(&go, 0);
    for (int i = 0; i < THREADS; i++) {
        args[i] = (struct perf_worker_arg) {
            .id = i,
            .go = &go,
        };
        tk_assert(pthread_create(&threads[i], NULL, perf_worker,
                                 &args[i]) == 0,
                  "pthread_create failed");
    }

    long long start = monotonic_ns();
    atomic_store_explicit(&go, 1, memory_order_release);

    for (int i = 0; i < THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0, "pthread_join failed");
    }

    long long elapsed_ms = (monotonic_ns() - start) / 1000000LL;
    tk_assert(elapsed_ms < 4500,
              "parallel throughput smoke took too long: %lld ms",
              elapsed_ms);
}

#endif

#ifdef LOCAL_VMALLOC_WRAP

static atomic_size_t vmalloc_total_bytes;
static atomic_size_t vmalloc_live_bytes;
static atomic_size_t vmalloc_calls;
static atomic_size_t vmalloc_live_limit;

void *__real_vmalloc(void *addr, size_t length);
void __real_vmfree(void *addr, size_t length);

void *__wrap_vmalloc(void *addr, size_t length) {
    size_t limit = atomic_load_explicit(&vmalloc_live_limit,
                                        memory_order_relaxed);
    size_t live = atomic_load_explicit(&vmalloc_live_bytes,
                                       memory_order_relaxed);
    if (limit && live + length > limit) {
        return NULL;
    }

    void *ptr = __real_vmalloc(addr, length);
    if (ptr) {
        atomic_fetch_add_explicit(&vmalloc_calls, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&vmalloc_total_bytes, length,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(&vmalloc_live_bytes, length,
                                  memory_order_relaxed);
    }
    return ptr;
}

void __wrap_vmfree(void *addr, size_t length) {
    atomic_fetch_sub_explicit(&vmalloc_live_bytes, length,
                              memory_order_relaxed);
    __real_vmfree(addr, length);
}

static void vmstats_reset(void) {
    atomic_store_explicit(&vmalloc_total_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&vmalloc_live_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&vmalloc_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&vmalloc_live_limit, 0, memory_order_relaxed);
}

UnitTest(memory_usage_stays_within_local_budget) {
    enum { N = 4096, SIZE = 128 };
    static void *ptrs[N];
    size_t requested = 0;

    vmstats_reset();

    for (int i = 0; i < N; i++) {
        ptrs[i] = mymalloc(SIZE);
        assert_alloc_ok(ptrs[i], SIZE);
        requested += SIZE;
        fill_block(ptrs[i], SIZE, 0x600000u + (uint32_t)i);
    }

    size_t live = atomic_load_explicit(&vmalloc_live_bytes,
                                       memory_order_relaxed);
    tk_assert(live <= requested * 4 + 65536,
              "mapped too much memory: live=%zu requested=%zu",
              live, requested);

    for (int i = 0; i < N; i++) {
        check_block(ptrs[i], SIZE, 0x600000u + (uint32_t)i);
        myfree(ptrs[i]);
    }
}

UnitTest(freed_blocks_can_be_reused_under_vmalloc_limit) {
    enum { N = 32768, SIZE = 128 };
    static void *ptrs[N];

    vmstats_reset();

    for (int i = 0; i < N; i++) {
        ptrs[i] = mymalloc(SIZE);
        assert_alloc_ok(ptrs[i], SIZE);
        fill_block(ptrs[i], SIZE, 0x700000u + (uint32_t)i);
    }

    size_t first_live = atomic_load_explicit(&vmalloc_live_bytes,
                                             memory_order_relaxed);
    tk_assert(first_live > 0, "allocator did not use vmalloc");

    for (int i = 0; i < N; i++) {
        check_block(ptrs[i], SIZE, 0x700000u + (uint32_t)i);
        myfree(ptrs[i]);
    }

    atomic_store_explicit(&vmalloc_live_limit, first_live,
                          memory_order_relaxed);

    for (int i = 0; i < N; i++) {
        ptrs[i] = mymalloc(SIZE);
        assert_alloc_ok(ptrs[i], SIZE);
        fill_block(ptrs[i], SIZE, 0x710000u + (uint32_t)i);
    }

    for (int i = 0; i < N; i++) {
        check_block(ptrs[i], SIZE, 0x710000u + (uint32_t)i);
        myfree(ptrs[i]);
    }

    atomic_store_explicit(&vmalloc_live_limit, 0, memory_order_relaxed);
}

struct limited_reuse_arg {
    int id;
    atomic_int *go;
};

static void *limited_reuse_worker(void *arg_ptr) {
    enum { SLOTS = 48, OPS = 1800 };
    struct limited_reuse_arg *arg = arg_ptr;
    struct alloc_rec slots[SLOTS] = { 0 };
    uint32_t rng = 0x810000u + (uint32_t)arg->id * 65537u;

    while (!atomic_load_explicit(arg->go, memory_order_acquire)) {
    }

    for (int op = 0; op < OPS; op++) {
        int slot = (int)(rng_next(&rng) % SLOTS);
        if (slots[slot].ptr) {
            check_block(slots[slot].ptr, slots[slot].size, slots[slot].tag);
            myfree(slots[slot].ptr);
            slots[slot] = (struct alloc_rec){ 0 };
        } else {
            size_t size = (rng_next(&rng) % 256u) + 1u;
            uint32_t tag = 0x820000u + (uint32_t)arg->id * 100000u
                         + (uint32_t)op;
            void *ptr = mymalloc(size);
            assert_alloc_ok(ptr, size);
            slots[slot] = (struct alloc_rec) {
                .ptr = ptr,
                .size = size,
                .tag = tag,
            };
            fill_block(ptr, size, tag);
        }
    }

    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].ptr) {
            check_block(slots[i].ptr, slots[i].size, slots[i].tag);
            myfree(slots[i].ptr);
        }
    }

    return NULL;
}

UnitTest(concurrent_reuse_under_vmalloc_limit) {
    enum { WARMUP = 8192, SIZE = 128, THREADS = 8 };
    static void *warmup[WARMUP];
    pthread_t threads[THREADS];
    struct limited_reuse_arg args[THREADS];
    atomic_int go;

    vmstats_reset();

    for (int i = 0; i < WARMUP; i++) {
        warmup[i] = mymalloc(SIZE);
        assert_alloc_ok(warmup[i], SIZE);
        fill_block(warmup[i], SIZE, 0x830000u + (uint32_t)i);
    }

    size_t warmup_live = atomic_load_explicit(&vmalloc_live_bytes,
                                              memory_order_relaxed);
    tk_assert(warmup_live > 0, "allocator did not use vmalloc");

    for (int i = 0; i < WARMUP; i++) {
        check_block(warmup[i], SIZE, 0x830000u + (uint32_t)i);
        myfree(warmup[i]);
    }

    atomic_store_explicit(&vmalloc_live_limit, warmup_live,
                          memory_order_relaxed);
    atomic_init(&go, 0);

    for (int i = 0; i < THREADS; i++) {
        args[i] = (struct limited_reuse_arg) {
            .id = i,
            .go = &go,
        };
        tk_assert(pthread_create(&threads[i], NULL, limited_reuse_worker,
                                 &args[i]) == 0,
                  "pthread_create failed");
    }

    atomic_store_explicit(&go, 1, memory_order_release);

    for (int i = 0; i < THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0, "pthread_join failed");
    }

    atomic_store_explicit(&vmalloc_live_limit, 0, memory_order_relaxed);
}

struct fixed_reuse_arg {
    int id;
    atomic_int *go;
};

static void *fixed_reuse_worker(void *arg_ptr) {
    enum { SLOTS = 256, SIZE = 96 };
    struct fixed_reuse_arg *arg = arg_ptr;
    void *ptrs[SLOTS];

    while (!atomic_load_explicit(arg->go, memory_order_acquire)) {
    }

    for (int i = 0; i < SLOTS; i++) {
        ptrs[i] = mymalloc(SIZE);
        assert_alloc_ok(ptrs[i], SIZE);
        fill_block(ptrs[i], SIZE, 0x850000u + (uint32_t)arg->id * 10000u
                                + (uint32_t)i);
    }

    for (int i = SLOTS - 1; i >= 0; i--) {
        check_block(ptrs[i], SIZE, 0x850000u + (uint32_t)arg->id * 10000u
                                  + (uint32_t)i);
        myfree(ptrs[i]);
    }

    return NULL;
}

UnitTest(concurrent_fixed_size_reuse_under_vmalloc_limit) {
    enum { WARMUP = 8192, SIZE = 96, THREADS = 8 };
    static void *warmup[WARMUP];
    pthread_t threads[THREADS];
    struct fixed_reuse_arg args[THREADS];
    atomic_int go;

    vmstats_reset();

    for (int i = 0; i < WARMUP; i++) {
        warmup[i] = mymalloc(SIZE);
        assert_alloc_ok(warmup[i], SIZE);
        fill_block(warmup[i], SIZE, 0x860000u + (uint32_t)i);
    }

    size_t warmup_live = atomic_load_explicit(&vmalloc_live_bytes,
                                              memory_order_relaxed);
    tk_assert(warmup_live > 0, "allocator did not use vmalloc");

    for (int i = 0; i < WARMUP; i++) {
        check_block(warmup[i], SIZE, 0x860000u + (uint32_t)i);
        myfree(warmup[i]);
    }

    atomic_store_explicit(&vmalloc_live_limit, warmup_live,
                          memory_order_relaxed);
    atomic_init(&go, 0);

    for (int i = 0; i < THREADS; i++) {
        args[i] = (struct fixed_reuse_arg) {
            .id = i,
            .go = &go,
        };
        tk_assert(pthread_create(&threads[i], NULL, fixed_reuse_worker,
                                 &args[i]) == 0,
                  "pthread_create failed");
    }

    atomic_store_explicit(&go, 1, memory_order_release);

    for (int i = 0; i < THREADS; i++) {
        tk_assert(pthread_join(threads[i], NULL) == 0, "pthread_join failed");
    }

    atomic_store_explicit(&vmalloc_live_limit, 0, memory_order_relaxed);
}

#endif
