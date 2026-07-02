#include "mymalloc.h"
#include <assert.h>
#include <stdlib.h>

spinlock_t big_lock;

// We don't need this malloc_count. You can remove it.
long malloc_count;

chunk_header_t *chunk_header_head;

static inline size_t align(size_t size, size_t alignment) {
    return ((size) + (alignment - 1)) & ~(alignment - 1);
}

chunk_header_t *create_new_chunk(size_t size) {
    size = align(size + CHUNK_HEADER_SIZE + BLOCK_HEADER_SIZE,
                 PAGE_SIZE * 1024); // 4MB

    chunk_header_t *new_chunk_header = (chunk_header_t *)vmalloc(NULL, size);
    if (new_chunk_header == NULL) {
        return NULL;
    }

    block_header_t *first_block_header =
        (block_header_t *)new_chunk_header + CHUNK_HEADER_SIZE;
    first_block_header->block_size =
        size - CHUNK_HEADER_SIZE - BLOCK_HEADER_SIZE;
    first_block_header->is_free_block = 1;
    first_block_header->next_block_header = NULL;
    first_block_header->prev_block_header = NULL;

    new_chunk_header->first_block_header = first_block_header;
    new_chunk_header->next_chunk_header = NULL;
    new_chunk_header->prev_chunk_header = NULL;

    // 头插法
    if (chunk_header_head) {
        chunk_header_head->prev_chunk_header = new_chunk_header;
    }
    new_chunk_header->next_chunk_header = chunk_header_head;
    chunk_header_head = new_chunk_header;

    return new_chunk_header;
}

void split_block(block_header_t *block_header, size_t size) {
    assert(block_header->block_size >= size);

    size_t next_size = block_header->block_size - size - BLOCK_HEADER_SIZE;
    block_header_t *new_block_header =
        (block_header_t *)((uintptr_t)block_header + size + BLOCK_HEADER_SIZE);
    new_block_header->block_size = next_size;
    new_block_header->is_free_block = 1;
    new_block_header->next_block_header = NULL;
    new_block_header->prev_block_header = block_header;

    block_header->block_size = size;
    block_header->is_free_block = 0;
    block_header->next_block_header = new_block_header;
}

void *mymalloc(size_t size) {

    if (size <= 0) {
        return NULL;
    }

    spin_lock(&big_lock);
    // malloc_count++;

    size = align(size, ALIGNMENT);

    chunk_header_t *chunk_header = chunk_header_head;

    block_header_t *block_header = NULL;
    while (chunk_header) {
        block_header = chunk_header->first_block_header;
        while (block_header) {
            if (block_header->is_free_block) {
                if (block_header->block_size >= size) {
                    split_block(block_header, size);
                    spin_unlock(&big_lock);
                    return (void *)((uintptr_t)block_header +
                                    BLOCK_HEADER_SIZE);
                }
            }
            block_header = block_header->next_block_header;
        }
        chunk_header = chunk_header->next_chunk_header;
    }
    // 如果没有合适的chunk，创建新的chunk
    chunk_header_t *new_chunk_header = create_new_chunk(size);
    if (new_chunk_header == NULL) {
        return NULL;
    }
    block_header = new_chunk_header->first_block_header;

    new_chunk_header->next_chunk_header = chunk_header_head;
    if (chunk_header_head) {
        chunk_header_head->prev_chunk_header = new_chunk_header;
    }
    split_block(block_header, size);

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
    block_header->is_free_block = 1;
    spin_unlock(&big_lock);
}
