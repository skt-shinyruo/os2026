#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#define ALIGNMENT 8
#define ALIGN(size) (((size) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))

typedef struct {
    atomic_int status;
} spinlock_t;

#define LOCKED 1
#define UNLOCKED 0

static inline void spin_lock(spinlock_t *lock) {
    int expected;
    do {
        expected = UNLOCKED;
    } while (!atomic_compare_exchange_strong(&lock->status, &expected, LOCKED));
}

static inline void spin_unlock(spinlock_t *lock) {
    atomic_store_explicit(&lock->status, UNLOCKED, memory_order_release);
}

typedef struct chunk_header chunk_header_t;
typedef struct block_header block_header_t;

typedef struct block_header {
    size_t size;
    int free;
    chunk_header_t *chunk;
    block_header_t *prev;
    block_header_t *next;
    block_header_t *free_prev;
    block_header_t *free_next;
} block_header_t;

typedef struct chunk_header {
    size_t size;
    chunk_header_t *prev;
    chunk_header_t *next;
    block_header_t *first_block;
} chunk_header_t;

#define BLOCK_HEADER_SIZE ALIGN(sizeof(block_header_t))
#define CHUNK_HEADER_SIZE ALIGN(sizeof(chunk_header_t))

void *mymalloc(size_t size);
void myfree(void *ptr);

void *vmalloc(void *addr, size_t length);
void vmfree(void *addr, size_t length);
