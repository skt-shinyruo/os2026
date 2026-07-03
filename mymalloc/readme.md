# M5 本地测试说明

本目录提供几个显式的本地测试 target，用于避免直接运行默认 `make` 时触发课程框架的 `git-trace` 流程。

## `make local-check`

只做 freestanding 语法检查，不运行测试。

用途：检查 `mymalloc.c` 和 `start.c` 在 `-ffreestanding -nostdlib` 条件下有没有明显不合法依赖或编译错误。

它不会检查链接，也不会运行 malloc 行为测试。

## `make local-test`

普通本地测试。

用途：编译 hosted 版本，然后运行基础正确性和并发测试。

覆盖内容包括：

- 8 字节对齐
- 读写校验
- 活跃分配区间不重叠
- 顺序随机 `malloc/free`
- 多线程 `malloc/free`
- 跨线程 `free`
- 大块分配

## `make local-test-wrap`

增强本地测试。

和 `local-test` 的区别是额外启用：

```bash
-DLOCAL_VMALLOC_WRAP
-Wl,--wrap=vmalloc -Wl,--wrap=vmfree
```

用途：拦截 `vmalloc/vmfree`，统计 allocator 到底映射了多少内存，并可以设置 `vmalloc` 限额。

它可以测试：

- 内存浪费是否过大
- `myfree` 后是否真的复用
- 在不能继续 `vmalloc` 的情况下是否还能靠 free list 工作
- 并发复用是否安全

## `make local-test-perf`

最重的一组测试。

它等于 `local-test-wrap` 再额外启用：

```bash
-DLOCAL_PERF_SMOKE
```

用途：额外启用一个宽松的并发吞吐 smoke test。

它不是严格 benchmark，只是用来抓明显性能问题、死锁、严重锁竞争或内容破坏。

## 推荐运行顺序

```bash
make local-check
make local-test
make local-test-wrap
make local-test-perf
```

修 bug 时建议优先跑：

```bash
make local-test
```

等基础 correctness 稳定后，再跑：

```bash
make local-test-wrap
make local-test-perf
```
