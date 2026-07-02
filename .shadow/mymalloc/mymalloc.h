#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    atomic_int status;
} spinlock_t;

#define LOCKED 1
#define UNLOCKED 0

#define ALIGNMENT 8
#define PAGE_SIZE 4096

#define CHUNK_HEADER_SIZE sizeof(chunk_header_t)
#define BLOCK_HEADER_SIZE sizeof(block_header_t)

typedef struct chunk_header {
    struct block_header *first_block_header;

    struct chunk_header *next_chunk_header;
    struct chunk_header *prev_chunk_header;
} chunk_header_t;

typedef struct block_header {
    size_t block_size;
    int is_free_block;
    struct block_header *next_block_header;
    struct block_header *prev_block_header;
} block_header_t;

static inline void spin_lock(spinlock_t *lock) {
    int expected;
    do {
        expected = UNLOCKED;
    } while (!atomic_compare_exchange_strong(&lock->status, &expected, LOCKED));
}

static inline void spin_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->status, UNLOCKED, memory_order_release);
}

void *mymalloc(size_t size);
void myfree(void *ptr);

void *vmalloc(void *addr, size_t length);
void vmfree(void *addr, size_t length);
