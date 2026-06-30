#include <mymalloc.h>

spinlock_t big_lock;

static chunk_header_t *chunk_list = NULL;
static block_header_t *free_list = NULL;

static size_t chunk_total_size(size_t block_size) {
    if (block_size > SIZE_MAX - CHUNK_HEADER_SIZE - BLOCK_HEADER_SIZE) {
        return 0;
    }

    size_t required = CHUNK_HEADER_SIZE + BLOCK_HEADER_SIZE + block_size;
    if (required > SIZE_MAX - 4095u) {
        return 0;
    }

    return (required + 4095u) & ~(size_t)4095u;
}

static void free_list_remove(block_header_t *block) {
    if (block->free_prev != NULL) {
        block->free_prev->free_next = block->free_next;
    } else if (free_list == block) {
        free_list = block->free_next;
    }
    if (block->free_next != NULL) {
        block->free_next->free_prev = block->free_prev;
    }
    block->free_prev = NULL;
    block->free_next = NULL;
}

static void free_list_insert(block_header_t *block) {
    block->free_prev = NULL;
    block->free_next = free_list;
    if (free_list != NULL) {
        free_list->free_prev = block;
    }
    free_list = block;
}

static chunk_header_t *create_chunk(size_t size) {
    size_t total_size = chunk_total_size(size);
    if (total_size == 0) {
        return NULL;
    }

    chunk_header_t *chunk = (chunk_header_t *)vmalloc(NULL, total_size);
    if (chunk == NULL) {
        return NULL;
    }

    chunk->size = total_size;
    chunk->prev = NULL;
    chunk->next = NULL;

    block_header_t *block = (block_header_t *)((char *)chunk + CHUNK_HEADER_SIZE);
    block->size = total_size - CHUNK_HEADER_SIZE - BLOCK_HEADER_SIZE;
    block->free = 1;
    block->chunk = chunk;
    block->prev = NULL;
    block->next = NULL;
    block->free_prev = NULL;
    block->free_next = NULL;
    chunk->first_block = block;
    free_list_insert(block);
    return chunk;
}

static void split_block(block_header_t *block, size_t size) {
    size_t remain = block->size - size;
    if (remain < BLOCK_HEADER_SIZE + ALIGNMENT) {
        return;
    }

    block_header_t *next = (block_header_t *)((char *)block + BLOCK_HEADER_SIZE + size);
    next->size = remain - BLOCK_HEADER_SIZE;
    next->free = 1;
    next->chunk = block->chunk;
    next->prev = block;
    next->next = block->next;
    next->free_prev = NULL;
    next->free_next = NULL;

    if (block->next != NULL) {
        block->next->prev = next;
    }
    block->next = next;
    block->size = size;
    free_list_insert(next);
}

static block_header_t *find_fit(size_t size) {
    for (block_header_t *block = free_list; block != NULL; block = block->free_next) {
        if (block->size >= size) {
            return block;
        }
    }
    return NULL;
}

void *mymalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    if (size > SIZE_MAX - (ALIGNMENT - 1)) {
        return NULL;
    }
    size = ALIGN(size);

    spin_lock(&big_lock);

    block_header_t *block = find_fit(size);
    if (block == NULL) {
        chunk_header_t *chunk = create_chunk(size);
        if (chunk == NULL) {
            spin_unlock(&big_lock);
            return NULL;
        }

        if (chunk_list == NULL) {
            chunk_list = chunk;
        } else {
            chunk_header_t *tail = chunk_list;
            while (tail->next != NULL) {
                tail = tail->next;
            }
            tail->next = chunk;
            chunk->prev = tail;
        }
        block = chunk->first_block;
    }

    free_list_remove(block);
    block->free = 0;
    split_block(block, size);

    spin_unlock(&big_lock);
    return (void *)((char *)block + BLOCK_HEADER_SIZE);
}

static void try_coalesce_right(block_header_t *block) {
    block_header_t *right = block->next;
    if (right == NULL || !right->free) {
        return;
    }

    free_list_remove(right);
    block->size += BLOCK_HEADER_SIZE + right->size;
    block->next = right->next;
    if (right->next != NULL) {
        right->next->prev = block;
    }
}

static void try_coalesce_left(block_header_t **block_ptr) {
    block_header_t *block = *block_ptr;
    block_header_t *left = block->prev;
    if (left == NULL || !left->free) {
        return;
    }

    free_list_remove(left);
    left->size += BLOCK_HEADER_SIZE + block->size;
    left->next = block->next;
    if (block->next != NULL) {
        block->next->prev = left;
    }
    left->chunk->first_block = left;
    *block_ptr = left;
}

void myfree(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    spin_lock(&big_lock);

    block_header_t *block = (block_header_t *)((char *)ptr - BLOCK_HEADER_SIZE);
    block->free = 1;

    try_coalesce_right(block);
    try_coalesce_left(&block);
    block->free = 1;
    free_list_insert(block);

    spin_unlock(&big_lock);
}
