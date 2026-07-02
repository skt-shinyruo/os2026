
#include "mymalloc.h"

int main(int argc, char const *argv[]) {

    void *ptr = mymalloc(10);
    myfree(ptr);
    return 0;
}
