
#include "mymalloc.h"

int main(int argc, char const *argv[]) {

    void *ptr1 = mymalloc(10);
    void *ptr2 = mymalloc(10);
    void *ptr3 = mymalloc(10);
    myfree(ptr1);
    myfree(ptr2);
    myfree(ptr3);
    return 0;
}
