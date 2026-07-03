#include <mymalloc.h>

#define PAGE_SIZE       4096ul
#define ALIGNMENT       8ul
#define NUM_ARENAS      64
#define NUM_BINS        32
#define CHUNK_SIZE      (64ul * 1024ul)
#define BLOCK_MAGIC     0x20260505u
#define BLOCK_FREE      1u
#define BLOCK_USED      0u

typedef struct block block_t;
typedef struct arena arena_t;

struct block {
    size_t size;
    arena_t *arena;
    void *segment;
    size_t segment_size;
    block_t *prev_free;
    block_t *next_free;
    uint32_t magic;
    uint32_t flags;
};

struct arena {
    spinlock_t lock;
    block_t *bins[NUM_BINS];
};

static arena_t arenas[NUM_ARENAS] __attribute__((aligned(64)));
static atomic_uint next_arena;

#ifndef FREESTANDING
static _Thread_local unsigned local_arena;
static _Thread_local int local_arena_ready;
#endif

#define HEADER_SIZE ((sizeof(block_t) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define FOOTER_SIZE (sizeof(size_t))
#define OVERHEAD    (HEADER_SIZE + FOOTER_SIZE)
#define MIN_BLOCK   (OVERHEAD + ALIGNMENT)

static size_t align_up(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

static int checked_align(size_t value, size_t align, size_t *result) {
    if (value > SIZE_MAX - (align - 1)) {
        return 0;
    }
    *result = align_up(value, align);
    return 1;
}

static int checked_add(size_t a, size_t b, size_t *result) {
    if (a > SIZE_MAX - b) {
        return 0;
    }
    *result = a + b;
    return 1;
}

static int bin_index(size_t size) {
    int idx = 0;
    size_t limit = MIN_BLOCK;

    while (idx + 1 < NUM_BINS && limit < size && limit <= (SIZE_MAX >> 1)) {
        limit <<= 1;
        idx++;
    }
    return idx;
}

static void write_footer(block_t *block) {
    *(size_t *)((char *)block + block->size - FOOTER_SIZE) = block->size;
}

static arena_t *choose_arena(void) {
#ifdef FREESTANDING
    unsigned id = atomic_fetch_add_explicit(&next_arena, 1, memory_order_relaxed);
    return &arenas[id & (NUM_ARENAS - 1)];
#else
    if (!local_arena_ready) {
        unsigned id = atomic_fetch_add_explicit(&next_arena, 1, memory_order_relaxed);
        local_arena = id & (NUM_ARENAS - 1);
        local_arena_ready = 1;
    }
    return &arenas[local_arena];
#endif
}

static void insert_free(arena_t *arena, block_t *block) {
    int idx = bin_index(block->size);

    block->arena = arena;
    block->magic = BLOCK_MAGIC;
    block->flags = BLOCK_FREE;
    block->prev_free = NULL;
    block->next_free = arena->bins[idx];
    if (block->next_free) {
        block->next_free->prev_free = block;
    }
    arena->bins[idx] = block;
    write_footer(block);
}

static void unlink_free(arena_t *arena, block_t *block) {
    int idx = bin_index(block->size);

    if (block->prev_free) {
        block->prev_free->next_free = block->next_free;
    } else {
        arena->bins[idx] = block->next_free;
    }
    if (block->next_free) {
        block->next_free->prev_free = block->prev_free;
    }
    block->prev_free = NULL;
    block->next_free = NULL;
}

static block_t *find_free_block(arena_t *arena, size_t needed) {
    int start = bin_index(needed);

    for (int idx = start; idx < NUM_BINS; idx++) {
        for (block_t *block = arena->bins[idx]; block; block = block->next_free) {
            if (block->size >= needed) {
                unlink_free(arena, block);
                return block;
            }
        }
    }
    return NULL;
}

static void *block_payload(block_t *block) {
    return (char *)block + HEADER_SIZE;
}

static void split_and_mark_used(arena_t *arena, block_t *block, size_t needed) {
    size_t old_size = block->size;

    block->magic = BLOCK_MAGIC;
    block->flags = BLOCK_USED;
    block->prev_free = NULL;
    block->next_free = NULL;

    if (old_size >= needed + MIN_BLOCK) {
        block_t *rest = (block_t *)((char *)block + needed);
        rest->size = old_size - needed;
        rest->arena = arena;
        rest->segment = block->segment;
        rest->segment_size = block->segment_size;
        rest->prev_free = NULL;
        rest->next_free = NULL;
        rest->magic = BLOCK_MAGIC;
        rest->flags = BLOCK_FREE;

        block->size = needed;
        write_footer(block);
        insert_free(arena, rest);
    } else {
        block->size = old_size;
        write_footer(block);
    }
}

static block_t *next_phys_block(block_t *block) {
    char *segment_end = (char *)block->segment + block->segment_size;
    char *next = (char *)block + block->size;

    if (next >= segment_end) {
        return NULL;
    }
    return (block_t *)next;
}

static block_t *prev_phys_block(block_t *block) {
    char *segment_start = (char *)block->segment;

    if ((char *)block == segment_start) {
        return NULL;
    }

    size_t prev_size = *(size_t *)((char *)block - FOOTER_SIZE);
    if (prev_size < MIN_BLOCK || prev_size > (size_t)((char *)block - segment_start)) {
        return NULL;
    }
    return (block_t *)((char *)block - prev_size);
}

static block_t *coalesce(arena_t *arena, block_t *block) {
    block_t *next = next_phys_block(block);
    if (next && next->magic == BLOCK_MAGIC && next->flags == BLOCK_FREE) {
        unlink_free(arena, next);
        block->size += next->size;
        write_footer(block);
    }

    block_t *prev = prev_phys_block(block);
    if (prev && prev->magic == BLOCK_MAGIC && prev->flags == BLOCK_FREE) {
        unlink_free(arena, prev);
        prev->size += block->size;
        prev->flags = BLOCK_FREE;
        write_footer(prev);
        block = prev;
    }

    return block;
}

static block_t *request_segment(arena_t *arena, size_t needed) {
    size_t chunk_size = CHUNK_SIZE;
    if (chunk_size < needed) {
        if (!checked_align(needed, PAGE_SIZE, &chunk_size)) {
            return NULL;
        }
    }

    void *mem = vmalloc(NULL, chunk_size);
    if (!mem) {
        return NULL;
    }

    block_t *block = mem;
    block->size = chunk_size;
    block->arena = arena;
    block->segment = mem;
    block->segment_size = chunk_size;
    block->prev_free = NULL;
    block->next_free = NULL;
    block->magic = BLOCK_MAGIC;
    block->flags = BLOCK_FREE;
    write_footer(block);
    return block;
}

void *mymalloc(size_t size) {
    size_t payload_size;
    size_t needed;

    if (size == 0) {
        size = ALIGNMENT;
    }
    if (!checked_align(size, ALIGNMENT, &payload_size)) {
        return NULL;
    }
    if (!checked_add(payload_size, OVERHEAD, &needed)) {
        return NULL;
    }

    arena_t *arena = choose_arena();

    spin_lock(&arena->lock);
    block_t *block = find_free_block(arena, needed);
    if (block) {
        split_and_mark_used(arena, block, needed);
        spin_unlock(&arena->lock);
        return block_payload(block);
    }
    spin_unlock(&arena->lock);

    block = request_segment(arena, needed);
    if (!block) {
        return NULL;
    }

    spin_lock(&arena->lock);
    split_and_mark_used(arena, block, needed);
    spin_unlock(&arena->lock);
    return block_payload(block);
}

void myfree(void *ptr) {
    if (!ptr) {
        return;
    }

    block_t *block = (block_t *)((char *)ptr - HEADER_SIZE);
    arena_t *arena = block->arena;

    spin_lock(&arena->lock);
    if (block->magic != BLOCK_MAGIC || block->flags == BLOCK_FREE) {
        spin_unlock(&arena->lock);
        return;
    }

    block->flags = BLOCK_FREE;
    block = coalesce(arena, block);
    insert_free(arena, block);
    spin_unlock(&arena->lock);
}

#ifdef FREESTANDING
#define FREESTANDING_HEAP_SIZE (896ul * 1024ul)

static unsigned char freestanding_heap[FREESTANDING_HEAP_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static size_t freestanding_used;
static spinlock_t freestanding_lock;

void *vmalloc(void *addr, size_t length) {
    (void)addr;

    if (length == 0 || (length & (PAGE_SIZE - 1)) != 0) {
        return NULL;
    }

    spin_lock(&freestanding_lock);
    if (freestanding_used > FREESTANDING_HEAP_SIZE - length) {
        spin_unlock(&freestanding_lock);
        return NULL;
    }

    void *result = freestanding_heap + freestanding_used;
    freestanding_used += length;
    spin_unlock(&freestanding_lock);
    return result;
}

void vmfree(void *addr, size_t length) {
    (void)addr;
    (void)length;
}
#endif
