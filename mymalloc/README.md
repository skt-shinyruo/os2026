# mymalloc 代码阅读说明

`mymalloc` 是一个教学用的动态内存分配器，实现了类似 `malloc/free` 的
`mymalloc()` 和 `myfree()`，并通过 `vmalloc()/vmfree()` 抽象底层页级内存申请。
它的核心实现集中在 `mymalloc.c`，采用 8 字节对齐、分离空闲链表、边界标记合并和多
arena 自旋锁来支持并发测试。

本文档按源码阅读顺序解释实现，目标是帮助理解每个宏、结构体和函数在分配器中的作用。

## 目录结构

```text
mymalloc/
├── Makefile             # 构建、测试和 freestanding 编译检查
├── main.c               # 非 FREESTANDING 模式下的空 main
├── mymalloc.c           # 分配器主体
├── mymalloc.h           # 对外 API 和自旋锁定义
├── start.c              # vmalloc/vmfree 的宿主环境实现，以及 FREESTANDING 入口
└── tests/
    ├── main.c
    ├── tests-m5.c       # M5 相关单元测试
    └── tests-trivial.c  # 示例和基础测试
```

主要阅读顺序建议：

1. `mymalloc.h`：先看对外接口和 `spinlock_t`。
2. `mymalloc.c` 顶部：看常量、`block_t`、`arena_t` 和块布局宏。
3. `mymalloc()`：理解分配主流程。
4. `myfree()`、`coalesce()`：理解释放和相邻空闲块合并。
5. `request_segment()`、`vmalloc()`：理解分配器如何向底层要内存。
6. `tests/`：对照测试理解实现需要满足的行为。

## 对外接口

`mymalloc.h` 暴露四个函数：

```c
void *mymalloc(size_t size);
void myfree(void *ptr);

void *vmalloc(void *addr, size_t length);
void vmfree(void *addr, size_t length);
```

含义如下：

- `mymalloc(size)`：申请至少 `size` 字节的用户可用内存，返回 8 字节对齐的指针。失败时返回
  `NULL`。`size == 0` 会被当作一次最小分配处理。
- `myfree(ptr)`：释放此前由 `mymalloc()` 返回的指针。`ptr == NULL` 直接返回。
- `vmalloc(addr, length)`：页级内存申请接口。`mymalloc.c` 只依赖这个接口，不直接调用
  `mmap()`。
- `vmfree(addr, length)`：页级释放接口。当前分配器不会在 `myfree()` 时把整段内存归还给
  `vmfree()`。

头文件里还定义了一个简单自旋锁：

```c
typedef struct {
    atomic_int status;
} spinlock_t;
```

`spin_lock()` 通过 `atomic_compare_exchange_strong()` 把 `UNLOCKED` 改成 `LOCKED`；
`spin_unlock()` 用 `atomic_store_explicit(..., memory_order_release)` 释放锁。

## 全局常量

`mymalloc.c` 顶部的宏决定了分配器的基本行为：

```c
#define PAGE_SIZE       4096ul
#define ALIGNMENT       8ul
#define NUM_ARENAS      64
#define NUM_BINS        32
#define CHUNK_SIZE      (64ul * 1024ul)
#define BLOCK_MAGIC     0x20260505u
#define BLOCK_FREE      1u
#define BLOCK_USED      0u
```

- `PAGE_SIZE`：底层页大小，`request_segment()` 请求大块内存时按页对齐。
- `ALIGNMENT`：用户返回指针的对齐粒度，目前是 8 字节。
- `NUM_ARENAS`：arena 数量。arena 是带锁的分配区域，用来降低多线程竞争。
- `NUM_BINS`：每个 arena 中空闲链表桶的数量。
- `CHUNK_SIZE`：默认向 `vmalloc()` 申请 64 KiB。小请求从 64 KiB 段里切分，大请求会申请更大的页对齐段。
- `BLOCK_MAGIC`：块头中的魔数，用于做基本合法性检查。
- `BLOCK_FREE` / `BLOCK_USED`：块状态。当前实现中空闲为 `1`，使用中为 `0`。

## 核心数据结构

### block_t

每个分配块前面都有一个 `block_t` 头：

```c
struct block {
    size_t size;
    arena_t *arena;
    void *segment;
    size_t segment_size;
    block_t *prev_free;
    block_t *next_free;
    uint32_t magic;
    uint32_t flags;
};
```

字段含义：

- `size`：整个块的大小，包括块头、用户 payload 和尾部 footer。
- `arena`：这个块归属的 arena。释放时通过它找到对应的锁和空闲链表。
- `segment`：底层 `vmalloc()` 返回的段起始地址。
- `segment_size`：底层段大小。用于判断物理相邻的下一个块是否越过段尾。
- `prev_free` / `next_free`：空闲链表双向指针。只有空闲块在链表里。
- `magic`：魔数，释放时做粗略校验。
- `flags`：记录块当前是空闲还是已分配。

### arena_t

```c
struct arena {
    spinlock_t lock;
    block_t *bins[NUM_BINS];
};
```

每个 arena 有一把锁和 32 个空闲链表桶。`arenas` 是 64 个 arena 的全局数组，并且按 64 字节对齐：

```c
static arena_t arenas[NUM_ARENAS] __attribute__((aligned(64)));
static atomic_uint next_arena;
```

在普通宿主环境下，还有线程局部变量：

```c
static _Thread_local unsigned local_arena;
static _Thread_local int local_arena_ready;
```

这表示每个线程第一次分配时选择一个 arena，之后该线程固定使用这个 arena。这样多数情况下不同线程会落到不同 arena，减少锁竞争。

在 `FREESTANDING` 模式下没有线程局部变量，`choose_arena()` 每次调用都用 `next_arena` 轮询选择 arena。

## 块布局

每个块在内存中的布局如下：

```text
block 起始地址
│
├── block_t header，大小向 8 字节对齐，记为 HEADER_SIZE
│
├── payload，返回给调用者的区域，大小按 8 字节对齐
│
└── footer，一个 size_t，保存当前块总大小
```

相关宏：

```c
#define HEADER_SIZE ((sizeof(block_t) + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1))
#define FOOTER_SIZE (sizeof(size_t))
#define OVERHEAD    (HEADER_SIZE + FOOTER_SIZE)
#define MIN_BLOCK   (OVERHEAD + ALIGNMENT)
```

其中：

- `HEADER_SIZE`：块头大小，向 8 字节取整。
- `FOOTER_SIZE`：尾部保存一个 `size_t`。
- `OVERHEAD`：分配一个块必须额外消耗的元数据大小。
- `MIN_BLOCK`：可独立存在的最小块大小，必须能容纳元数据和至少 8 字节 payload。

在常见 64 位环境中，`sizeof(block_t)` 通常是 56，所以 `HEADER_SIZE` 是 56，`FOOTER_SIZE`
是 8，`OVERHEAD` 是 64，`MIN_BLOCK` 是 72。源码没有硬编码这些数值，而是用宏根据结构体大小计算。

footer 是实现向前合并的关键。给定一个块的起始地址，可以从 `block - FOOTER_SIZE` 读出前一个物理块的大小，再反推出前一个块的起始地址。

## 对齐和溢出检查

相关函数：

```c
static size_t align_up(size_t value, size_t align);
static int checked_align(size_t value, size_t align, size_t *result);
static int checked_add(size_t a, size_t b, size_t *result);
```

`align_up()` 使用位运算把数值向上对齐：

```c
(value + align - 1) & ~(align - 1)
```

这个写法要求 `align` 是 2 的幂。当前调用只传入 `8` 和 `4096`，都满足条件。

`checked_align()` 和 `checked_add()` 的职责是防止 `size_t` 溢出：

- `checked_align()` 在执行 `value + align - 1` 前检查是否会超过 `SIZE_MAX`。
- `checked_add()` 在计算 `payload_size + OVERHEAD` 前检查是否会超过 `SIZE_MAX`。

如果溢出，`mymalloc()` 返回 `NULL`。

## 空闲链表和 size class

每个 arena 里有 `NUM_BINS` 个桶，每个桶是一条空闲块双向链表。块根据 `bin_index(size)` 进入对应桶：

```c
static int bin_index(size_t size) {
    int idx = 0;
    size_t limit = MIN_BLOCK;

    while (idx + 1 < NUM_BINS && limit < size && limit <= (SIZE_MAX >> 1)) {
        limit <<= 1;
        idx++;
    }
    return idx;
}
```

这个函数从 `MIN_BLOCK` 开始，每次把上限翻倍，所以桶大致表示：

```text
bin 0: <= MIN_BLOCK
bin 1: <= MIN_BLOCK * 2
bin 2: <= MIN_BLOCK * 4
...
```

查找空闲块时，`find_free_block()` 会从目标大小对应的桶开始，向更大的桶查找：

```c
for (int idx = start; idx < NUM_BINS; idx++) {
    for (block_t *block = arena->bins[idx]; block; block = block->next_free) {
        if (block->size >= needed) {
            unlink_free(arena, block);
            return block;
        }
    }
}
```

这是一种简单的 segregated free list 策略：

- 桶之间按大小分组，减少全局线性扫描。
- 桶内使用 first-fit，找到第一个足够大的块就取出。
- `insert_free()` 总是把空闲块插到链表头部，所以刚释放的块通常能很快被复用。

## 分配流程：mymalloc()

`mymalloc()` 的主流程如下：

```text
mymalloc(size)
│
├── 1. size == 0 时改成 8
├── 2. payload_size = align_up(size, 8)
├── 3. needed = payload_size + HEADER_SIZE + FOOTER_SIZE
├── 4. choose_arena()
├── 5. 加锁，在 arena 的 bins 中找空闲块
│   ├── 找到：取出，必要时切分，标记已使用，解锁返回 payload
│   └── 没找到：解锁
├── 6. request_segment() 向 vmalloc() 申请新段
├── 7. 重新加锁，把新段作为一个大空闲块切出需要的块
└── 8. 返回 payload
```

关键点：

1. 用户请求大小不直接作为块大小。源码先把用户 payload 向 8 字节对齐，再加上块头和 footer。
2. 先查空闲链表，找不到才向底层申请新段。
3. `request_segment()` 在 arena 锁外调用。这样可以避免持有 arena 锁时执行较慢的页级内存申请。
4. 新段返回后会重新加锁，再调用 `split_and_mark_used()`。

返回给用户的指针由 `block_payload()` 计算：

```c
return (char *)block + HEADER_SIZE;
```

因为 `HEADER_SIZE` 已按 8 字节对齐，且 `vmalloc()` 返回页对齐地址，所以 payload 也是 8 字节对齐的。

## 申请新段：request_segment()

当当前 arena 没有合适空闲块时，`request_segment()` 会向 `vmalloc()` 申请一个新 segment：

```c
size_t chunk_size = CHUNK_SIZE;
if (chunk_size < needed) {
    checked_align(needed, PAGE_SIZE, &chunk_size);
}
```

规则是：

- 默认申请 64 KiB。
- 如果单次分配需要超过 64 KiB，则把 `needed` 向 4096 字节页大小对齐，申请更大的段。

`vmalloc()` 返回后，整段内存先被初始化成一个大的空闲块：

```c
block->size = chunk_size;
block->arena = arena;
block->segment = mem;
block->segment_size = chunk_size;
block->magic = BLOCK_MAGIC;
block->flags = BLOCK_FREE;
write_footer(block);
```

这个大块不会先插入空闲链表，而是由 `mymalloc()` 随后直接调用 `split_and_mark_used()` 切出当前请求所需的块；剩余部分如果足够大，会被插入空闲链表。

## 切分块：split_and_mark_used()

`split_and_mark_used(arena, block, needed)` 的职责是把一个空闲块变成已分配块，并在空间足够时切出剩余空闲块。

流程：

```text
old_size = block->size
标记 block 为 BLOCK_USED

if old_size >= needed + MIN_BLOCK:
    block 占用 needed 字节
    rest = block + needed
    rest 占用 old_size - needed 字节
    rest 标记为空闲块并插入 bins
else:
    block 使用整个 old_size
```

为什么要检查 `old_size >= needed + MIN_BLOCK`？

因为剩余空间如果小于 `MIN_BLOCK`，它无法形成一个合法的独立块。此时直接把整块都给当前分配，避免产生不可管理的碎片。

切分后：

- 已分配块会写 footer。
- 剩余空闲块通过 `insert_free()` 写入链表，并写 footer。
- 已分配块不在任何空闲链表中。

## 释放流程：myfree()

`myfree()` 的主流程如下：

```text
myfree(ptr)
│
├── 1. ptr == NULL 直接返回
├── 2. block = ptr - HEADER_SIZE
├── 3. arena = block->arena
├── 4. 加 arena 锁
├── 5. 检查 magic 和 flags
│   ├── magic 不对：返回
│   └── 已经是 BLOCK_FREE：返回
├── 6. 标记当前块为空闲
├── 7. coalesce() 和前后物理相邻空闲块合并
├── 8. insert_free() 把合并后的块插入空闲链表
└── 9. 解锁
```

`myfree(NULL)` 是安全的。对同一个有效指针重复释放时，第二次会看到 `flags == BLOCK_FREE` 并返回。

需要注意：这不是一个完整的安全分配器。`myfree()` 在验证 `magic` 前需要通过 `ptr - HEADER_SIZE` 读出
`block->arena`，所以传入任意非法指针仍可能导致未定义行为或崩溃。它只面向正常调用路径和课程测试。

## 相邻块合并：coalesce()

释放时，如果相邻物理块也是空闲块，分配器会把它们合并成一个更大的空闲块，降低外部碎片。

### 找后一个块

`next_phys_block()` 通过当前块大小计算后一个块：

```c
char *segment_end = (char *)block->segment + block->segment_size;
char *next = (char *)block + block->size;

if (next >= segment_end) {
    return NULL;
}
return (block_t *)next;
```

如果 `next` 已到达或超过 segment 尾部，就说明没有后继块。

### 找前一个块

`prev_phys_block()` 使用 footer：

```c
size_t prev_size = *(size_t *)((char *)block - FOOTER_SIZE);
return (block_t *)((char *)block - prev_size);
```

为了避免明显错误，它会检查：

- 当前块是否已经是 segment 的第一个块。
- `prev_size` 是否小于 `MIN_BLOCK`。
- `prev_size` 是否超过当前块到 segment 起点的距离。

### 合并顺序

`coalesce()` 先尝试合并后一个空闲块，再尝试合并前一个空闲块：

```text
当前 block
├── 如果 next 是空闲块：
│   ├── 从 bins 里 unlink next
│   └── block->size += next->size
└── 如果 prev 是空闲块：
    ├── 从 bins 里 unlink prev
    ├── prev->size += block->size
    └── 返回 prev
```

被合并的相邻空闲块原本已经在空闲链表中，所以合并前必须先 `unlink_free()`，否则链表里会残留指向旧块的节点。

合并完成后，`myfree()` 会把最终的大块重新 `insert_free()` 到合适的 bin。

## vmalloc 和运行环境

`mymalloc.c` 通过 `vmalloc()` 获取底层内存。具体实现分两种模式。

### 普通宿主环境

`start.c` 在非 `FREESTANDING` 模式下用 `mmap()` 实现：

```c
void *result = mmap(addr, length, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
```

失败时返回 `NULL`。`vmfree()` 则调用 `munmap()`。

这种模式用于本地测试，例如：

```sh
make test-local
```

### FREESTANDING 环境

`mymalloc.c` 在 `#ifdef FREESTANDING` 下提供自己的 `vmalloc()`：

```c
#define FREESTANDING_HEAP_SIZE (896ul * 1024ul)

static unsigned char freestanding_heap[FREESTANDING_HEAP_SIZE]
    __attribute__((aligned(PAGE_SIZE)));
static size_t freestanding_used;
static spinlock_t freestanding_lock;
```

它使用一个 896 KiB 的静态数组作为堆，`vmalloc()` 是简单 bump pointer：

```text
检查 length 非 0 且页对齐
加 freestanding_lock
如果剩余空间不足，返回 NULL
result = freestanding_heap + freestanding_used
freestanding_used += length
解锁并返回 result
```

`vmfree()` 在这个模式下是空函数，不回收底层静态堆空间。也就是说：

- `myfree()` 会让块回到分配器自己的空闲链表。
- 但 `vmalloc()` 已经增长过的 `freestanding_used` 不会减少。
- 分配器可以复用已释放块，但不会把整个 segment 还给底层 freestanding 堆。

`Makefile` 的 `check` 目标会用 `-DFREESTANDING -ffreestanding -static -nostdlib -nostartfiles -no-pie`
编译，检查代码是否能在 freestanding 条件下链接。

## 并发模型

并发支持由两层机制组成：

1. 多 arena：全局有 64 个 `arena_t`，每个 arena 有独立空闲链表和锁。
2. 自旋锁：每次操作某个 arena 的空闲链表时，需要持有该 arena 的 `spinlock_t`。

普通宿主环境下，每个线程第一次调用 `mymalloc()` 时选择一个 arena，之后复用这个 arena。选择逻辑使用：

```c
atomic_fetch_add_explicit(&next_arena, 1, memory_order_relaxed)
```

然后通过：

```c
id & (NUM_ARENAS - 1)
```

把 id 映射到 0 到 63。这个写法依赖 `NUM_ARENAS` 是 2 的幂，当前值 64 满足条件。

`mymalloc()` 查找和修改空闲链表时持锁；找不到空闲块后会先释放锁，再调用 `request_segment()`。这样可以减少锁持有时间。

## 主要不变量

阅读或修改代码时，可以用以下不变量检查逻辑是否成立：

- 所有块的 `size` 都表示整个块大小，而不是用户 payload 大小。
- `size` 应至少为 `MIN_BLOCK`，并且通常保持 8 字节对齐。
- `block_payload(block) == (char *)block + HEADER_SIZE`。
- 同一个 segment 内的物理块首尾相接，不能跨越 `segment + segment_size`。
- 每个合法块的 footer 都保存当前块的 `size`。
- 空闲块必须在且只在一个 arena 的某个 bin 链表中。
- 已分配块不能出现在空闲链表中。
- `prev_free` / `next_free` 只对空闲块有意义。
- 合并相邻空闲块前，必须先把被合并的块从空闲链表移除。
- 访问或修改某个 arena 的 bin 链表时，需要持有该 arena 的锁。

## 测试覆盖的行为

`tests/tests-m5.c` 重点覆盖：

- 返回指针必须 8 字节对齐。
- 多个分配块不能互相重叠。
- payload 写入后在释放前必须保持不变。
- 刚释放的块应能被再次分配复用。
- 大分配，例如 200000 字节，应能成功并保持对齐。
- 多线程高频随机分配和释放不会破坏 payload。

`tests/tests-trivial.c` 覆盖：

- 基础 `mymalloc()` / `myfree()` 使用。
- `vmalloc()` 返回页对齐且不同的地址。
- 多线程循环分配释放的基本压力测试。

常用命令：

```sh
make test-local
make check
```

`make test-local` 会把 `mymalloc.c`、`start.c`、测试文件和 `testkit` 编译成本地测试程序并运行。
`make check` 主要检查 freestanding 编译条件。

## 局限和需要注意的点

这个分配器适合课程实验和理解 allocator 设计，但不是生产级实现：

- 不实现 `calloc()`、`realloc()` 或更强的对齐接口。
- 只保证当前实现中的 8 字节对齐，不保证满足所有平台上 `max_align_t` 的要求。
- `myfree()` 不能安全处理任意非法指针。
- `myfree()` 不会把整个空闲 segment 归还给 `vmfree()`。
- freestanding 版本的 `vmfree()` 是空操作，底层静态堆只增不减。
- 空闲链表桶内是简单 first-fit，长时间运行后可能存在碎片和扫描成本。
- `spin_lock()` 是忙等锁，临界区应保持较短；大量线程竞争同一个 arena 时会浪费 CPU。

## 从一次分配和释放串起来看

假设调用：

```c
void *p = mymalloc(128);
myfree(p);
```

分配时：

1. `128` 已经是 8 字节对齐，所以 `payload_size = 128`。
2. `needed = 128 + OVERHEAD`。
3. 选择当前线程的 arena。
4. 如果 bins 里没有合适空闲块，就通过 `vmalloc()` 申请至少 64 KiB。
5. 新 segment 被初始化为一个大空闲块。
6. `split_and_mark_used()` 从大块头部切出 `needed` 字节给用户。
7. 剩余空间形成新的空闲块，插入合适 bin。
8. 返回 `block + HEADER_SIZE`。

释放时：

1. `myfree()` 用 `p - HEADER_SIZE` 找回块头。
2. 根据块头里的 `arena` 加锁。
3. 检查魔数和状态。
4. 标记为空闲。
5. 如果前后物理相邻块也是空闲，就合并。
6. 把最终空闲块插回对应 bin。

这就是整套实现的核心闭环：底层按页拿大段，内部分割成块；释放时把块放回空闲链表，并尽量与相邻空闲块合并。
