#include "mymalloc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

spinlock_t big_lock;

// We don't need this malloc_count. You can remove it.
long malloc_count;

chunk_header_t *chunk_header_head;

static inline size_t align(size_t size, size_t alignment) {
    return ((size) + (alignment - 1)) & ~(alignment - 1);
}

chunk_header_t *create_new_chunk(size_t size) {
    size = align(size + CHUNK_HEADER_SIZE,
                 PAGE_SIZE * 1024); // 4MB
    chunk_header_t *new_chunk_header = (chunk_header_t *)vmalloc(NULL, size);
    if (new_chunk_header == NULL) {
        return NULL;
    }

    block_header_t *first_block_header =
        (block_header_t *)((uintptr_t)new_chunk_header + CHUNK_HEADER_SIZE);
    first_block_header->block_size =
        size - CHUNK_HEADER_SIZE - BLOCK_HEADER_SIZE;
    first_block_header->is_free_block = 1;
    first_block_header->next_block_header = NULL;
    first_block_header->prev_block_header = NULL;

    new_chunk_header->first_block_header = first_block_header;
    new_chunk_header->next_chunk_header = NULL;
    new_chunk_header->prev_chunk_header = NULL;

    // 头插法
    chunk_header_t *current_chunk_header = chunk_header_head;
    if (current_chunk_header) {
        current_chunk_header->prev_chunk_header = new_chunk_header;
    }
    new_chunk_header->next_chunk_header = current_chunk_header;
    chunk_header_head = new_chunk_header;

    printf("chunk: size=%zu chunk=%p first=%p payload=%zu end=%p\n", size,
           (void *)new_chunk_header, (void *)first_block_header,
           first_block_header->block_size,
           (void *)((uintptr_t)new_chunk_header + size));

    return new_chunk_header;
}

void split_block(block_header_t *block_header, size_t size) {
    assert(block_header->block_size >= size);

    size_t remaining_size = block_header->block_size - size;
    block_header_t *next_block_header = block_header->next_block_header;

    printf("split: block=%p old=%zu need=%zu rem=%td/%zu old_end=%p new=%p "
           "next=%p\n",
           (void *)block_header, block_header->block_size, size,
           (ptrdiff_t)block_header->block_size - (ptrdiff_t)size -
               (ptrdiff_t)BLOCK_HEADER_SIZE,
           remaining_size,
           (void *)((uintptr_t)block_header + BLOCK_HEADER_SIZE +
                    block_header->block_size),
           (void *)((uintptr_t)block_header + size), (void *)next_block_header);

    if (remaining_size < BLOCK_HEADER_SIZE) {
        printf("split no: block=%p keep=%zu need=%zu rem=%zu\n",
               (void *)block_header, block_header->block_size, size,
               remaining_size);
        block_header->block_size = size;
        block_header->is_free_block = 0;
        return;
    }

    block_header_t *new_block_header =
        (block_header_t *)((uintptr_t)block_header + size);
    new_block_header->block_size = remaining_size;
    new_block_header->is_free_block = 1;
    new_block_header->prev_block_header = block_header;
    new_block_header->next_block_header = next_block_header;

    block_header->block_size = size - BLOCK_HEADER_SIZE;
    block_header->is_free_block = 0;
    block_header->next_block_header = new_block_header;

    printf("split ok: used=%p payload=%zu free=%p free_payload=%zu "
           "free_end=%p next=%p\n",
           (void *)block_header, block_header->block_size,
           (void *)new_block_header, new_block_header->block_size,
           (void *)((uintptr_t)new_block_header + BLOCK_HEADER_SIZE +
                    new_block_header->block_size),
           (void *)new_block_header->next_block_header);
}

block_header_t *find_free_block(size_t size) {
    chunk_header_t *chunk_header = chunk_header_head;
    while (chunk_header) {
        block_header_t *current_block_header = chunk_header->first_block_header;
        while (current_block_header) {
            if (current_block_header->is_free_block &&
                current_block_header->block_size >= size) {
                printf("find: need=%zu choose=%p payload=%zu next=%p\n", size,
                       (void *)current_block_header,
                       current_block_header->block_size,
                       (void *)current_block_header->next_block_header);
                return current_block_header;
            }
            current_block_header = current_block_header->next_block_header;
        }
        chunk_header = chunk_header->next_chunk_header;
    }
    return NULL;
}

void *mymalloc(size_t size) {

    if (size <= 0) {
        return NULL;
    }
    printf("malloc: user=%zu\n", size);
    spin_lock(&big_lock);
    // malloc_count++;

    size = align(size + BLOCK_HEADER_SIZE, ALIGNMENT);
    printf("malloc aligned: total=%zu payload=%zu head=%p\n", size,
           size >= BLOCK_HEADER_SIZE ? size - BLOCK_HEADER_SIZE : 0,
           (void *)chunk_header_head);

    chunk_header_t *chunk_header = chunk_header_head;

    block_header_t *block_header = find_free_block(size);
    if (block_header == NULL) {
        // 如果没有合适的chunk，创建新的chunk
        chunk_header_t *new_chunk_header = create_new_chunk(size);
        if (new_chunk_header == NULL) {
            return NULL;
        }

        block_header = new_chunk_header->first_block_header;
    }
    assert(block_header->is_free_block);
    assert(block_header->block_size >= size);
    split_block(block_header, size);
    printf("malloc ret: ptr=%p block=%p payload=%zu next=%p\n",
           (void *)((uintptr_t)block_header + BLOCK_HEADER_SIZE),
           (void *)block_header, block_header->block_size,
           (void *)block_header->next_block_header);

    spin_unlock(&big_lock);

    return (void *)((uintptr_t)block_header + BLOCK_HEADER_SIZE);
}

void myfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    spin_lock(&big_lock);
    block_header_t *block_header =
        (block_header_t *)((uintptr_t)ptr - BLOCK_HEADER_SIZE);
    printf("free: ptr=%p block=%p payload=%zu was_free=%d end=%p next=%p\n",
           ptr, (void *)block_header, block_header->block_size,
           block_header->is_free_block,
           (void *)((uintptr_t)block_header + BLOCK_HEADER_SIZE +
                    block_header->block_size),
           (void *)block_header->next_block_header);
    block_header->is_free_block = 1;
    spin_unlock(&big_lock);
}
