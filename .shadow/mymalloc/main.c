
#include "mymalloc.h"

struct alloc_rec {
    void *ptr;
    size_t size;
    uint32_t tag;
};

int main(int argc, char const *argv[]) {

    enum { SLOTS = 256, OPS = 12000 };
    struct alloc_rec slots[SLOTS] = {0};
    uint32_t rng = 0x12345678u;

    for (int op = 0; op < OPS; op++) {
        size_t slot = rng_next(&rng) % SLOTS;
        if (slots[slot].ptr && (rng_next(&rng) & 1u)) {
            check_block(slots[slot].ptr, slots[slot].size, slots[slot].tag);
            myfree(slots[slot].ptr);
            slots[slot] = (struct alloc_rec){0};
            continue;
        }

        if (!slots[slot].ptr) {
            size_t size = (rng_next(&rng) % 512u) + 1u;
            void *ptr = mymalloc(size);
            assert_alloc_ok(ptr, size);
            check_no_overlap_with(slots, SLOTS, ptr, size);
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
            check_block(slots[i].ptr, slots[i].size, slots[i].tag);
            myfree(slots[i].ptr);
        }
    }
    return 0;
}
