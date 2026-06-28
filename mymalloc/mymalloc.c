#include <mymalloc.h>

spinlock_t big_lock;

block_header_t *header = NULL;

void *mymalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    spin_lock(&big_lock);

    block_header_t *current_header = header;

    // 遍历free list
    // 如果找到合适的块，分配内存并返回指针
    while (current_header != NULL) {
        if (current_header->free && current_header->size >= size) {
            current_header->free = 0;
            spin_unlock(&big_lock);
            return (void *)((char *)current_header + HEADER_SIZE);
        }
        current_header = current_header->next;
    }

    // 如果没有找到合适的块，再次分配一块并添加到末尾
    size_t new_size = ALIGN(size) + HEADER_SIZE;
    block_header_t *new_header = (block_header_t *)vmalloc(NULL, new_size);
    if (new_header == NULL) {
        spin_unlock(&big_lock);
        return NULL;
    }

    new_header->size = new_size;
    new_header->next = NULL;
    new_header->free = 0;
    if (header == NULL) {
        header = new_header;
    } else {
        current_header = header;
        while (current_header->next != NULL) {
            current_header = current_header->next;
        }
        current_header->next = new_header;
    }

    spin_unlock(&big_lock);
    return (void *)((char *)new_header + HEADER_SIZE);
}

void myfree(void *ptr) {
    spin_lock(&big_lock);

    block_header_t *current_header =
        (block_header_t *)((char *)ptr - HEADER_SIZE);
    current_header->free = 1;

    spin_unlock(&big_lock);
}
