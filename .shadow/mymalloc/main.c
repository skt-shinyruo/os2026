#include "mymalloc.h"

#include <stdint.h>
#include <stdio.h>

#define SLOTS 32
#define OPS 1000

struct alloc_rec {
    void *ptr;
    size_t size;
    uint32_t tag;
};

static uint32_t rng_next(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9e3779b9u;
    return *state;
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

static int check_block(void *ptr, size_t size, uint32_t tag) {
    uint8_t *p = ptr;
    for (size_t i = 0; i < size; i++) {
        uint8_t expected = pattern_byte(tag, i);
        if (p[i] != expected) {
            printf("corrupted block: ptr=%p size=%zu offset=%zu got=%u "
                   "expected=%u\n",
                   ptr, size, i, p[i], expected);
            return 0;
        }
    }
    return 1;
}

static int ranges_overlap(void *a, size_t asize, void *b, size_t bsize) {
    uintptr_t lo_a = (uintptr_t)a;
    uintptr_t hi_a = lo_a + asize;
    uintptr_t lo_b = (uintptr_t)b;
    uintptr_t hi_b = lo_b + bsize;
    return lo_a < hi_b && lo_b < hi_a;
}

static int check_no_overlap(struct alloc_rec *slots, void *ptr, size_t size) {
    for (int i = 0; i < SLOTS; i++) {
        if (!slots[i].ptr) {
            continue;
        }
        if (ranges_overlap(slots[i].ptr, slots[i].size, ptr, size)) {
            printf("overlap: slot %d old=%p+%zu new=%p+%zu\n", i, slots[i].ptr,
                   slots[i].size, ptr, size);
            return 0;
        }
    }
    return 1;
}

int main(void) {
    struct alloc_rec slots[SLOTS] = {0};
    uint32_t rng = 0x12345678u;

    setvbuf(stdout, NULL, _IONBF, 0);

    for (int op = 0; op < OPS; op++) {
        size_t slot = rng_next(&rng) % SLOTS;

        if (slots[slot].ptr && (rng_next(&rng) & 1u)) {
            printf("op %d: free slot %zu ptr=%p size=%zu\n", op, slot,
                   slots[slot].ptr, slots[slot].size);
            if (!check_block(slots[slot].ptr, slots[slot].size,
                             slots[slot].tag)) {
                return 1;
            }
            myfree(slots[slot].ptr);
            slots[slot] = (struct alloc_rec){0};
            continue;
        }

        if (!slots[slot].ptr) {
            size_t size = (rng_next(&rng) % 128u) + 1u;
            void *ptr = mymalloc(size);
            printf("op %d: alloc slot %zu ptr=%p size=%zu\n", op, slot, ptr,
                   size);
            if (!ptr) {
                printf("mymalloc(%zu) returned NULL\n", size);
                return 1;
            }
            if (!check_no_overlap(slots, ptr, size)) {
                return 1;
            }

            slots[slot] = (struct alloc_rec){
                .ptr = ptr,
                .size = size,
                .tag = 0x300000u + (uint32_t)op,
            };
            fill_block(ptr, size, slots[slot].tag);
        }
    }

    for (int i = 0; i < SLOTS; i++) {
        if (slots[i].ptr) {
            printf("cleanup: free slot %d ptr=%p size=%zu\n", i, slots[i].ptr,
                   slots[i].size);
            if (!check_block(slots[i].ptr, slots[i].size, slots[i].tag)) {
                return 1;
            }
            myfree(slots[i].ptr);
        }
    }

    printf("sequential random model completed\n");
    return 0;
}
