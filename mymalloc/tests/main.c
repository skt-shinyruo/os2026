#include <mymalloc.h>
#include <testkit.h>

int main(int argc, const char **argv, const char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    int *p1 = mymalloc(4);
    tk_assert(p1 != NULL, "malloc should not return NULL");
    *p1 = 1024;

    int *p2 = mymalloc(4);
    tk_assert(p2 != NULL, "malloc should not return NULL");
    *p2 = 2048;

    tk_assert(p1 != p2, "malloc should return different pointers");
    tk_assert(*p1 * 2 == *p2, "value check should pass");

    myfree(p1);
    myfree(p2);

    tk_assert(mymalloc(0) == NULL, "mymalloc(0) should return NULL");

    int *value = mymalloc(sizeof(*value));
    tk_assert(value != NULL, "mymalloc(sizeof(int)) should succeed");
    *value = 0x12345678;
    tk_assert(*value == 0x12345678, "allocated memory should be writable");
    myfree(value);

    int *first = mymalloc(sizeof(*first));
    tk_assert(first != NULL, "allocation before free should succeed");
    myfree(first);

    int *second = mymalloc(sizeof(*second));
    tk_assert(second != NULL, "allocation after free should succeed");
    tk_assert(second == first, "allocator should reuse a compatible freed block");
    myfree(second);

    void *aligned = mymalloc(3);
    tk_assert(aligned != NULL, "allocation for alignment check should succeed");
    tk_assert((uintptr_t)aligned % ALIGNMENT == 0,
              "returned pointer should satisfy allocator alignment");
    myfree(aligned);

    return 0;
}
