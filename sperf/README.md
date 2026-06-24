# M3 实现引导

M3 的核心不是“写一个很聪明的 profiler”，而是先把这条链路理顺：`你的程序 -> strace -> COMMAND`，然后只做三件事：收集、聚合、周期输出。

你可以按这个顺序自检：

1. 先确认子进程能用 `execve` 正确启动 `strace`，并把原始 `COMMAND ARG...` 交给它，同时别丢 `envp` / `PATH`。
2. 再确认父进程能稳定读到 `strace` 的输出，并在被 trace 的进程结束时一起退出。
3. 然后只把“一行完整的 syscall trace”当解析单元，先别急着做排序和百分比。
4. 最后再加统计、top 5、每 100ms 左右刷新一次、每轮后输出 80 个 `\0`，并及时 `fflush`。

调试时别反过来：先拿 `true`、`echo` 这种短命令把进程关系和解析跑通，再去碰长命令。

## 测试命令

先用这些命令验证 `sperf` 的进程关系和输出链路：

```bash
./sperf true
./sperf echo hello
./sperf sleep 3
./sperf sh -c 'sleep 1; sleep 1; sleep 1'
```

再用这些命令看不同 syscall 模式：

```bash
./sperf find /usr/include -type f
./sperf ls -R /usr/include
./sperf dd if=/dev/zero of=/dev/null bs=1M count=4096
./sperf grep -R "include" /usr/include
./sperf sha256sum /usr/bin/*
```

如果想测更久一点，可以把 `sleep` 或 `dd` 的参数调大；如果要测无限命令，外面套一层 `timeout`：

```bash
./sperf timeout 5 sh -c 'yes > /dev/null'
```

## 排查方法

如果 `./sperf ls` 直接崩溃，建议按下面顺序查。

1. 先拿最小复现，不要用复杂命令。

   ```bash
   cd /home/feng/code/os/jyy/os2026/sperf
   ./sperf true
   ./sperf ls
   ```

2. 用 `gdb` 拿到第一手栈，不要靠肉眼猜。

   ```bash
   gdb -q ./sperf -ex run -ex bt -ex quit --args ./sperf ls
   ```

   你要关注的是：

   - 崩在哪一行
   - `bt` 里 `main()` 对应哪一行
   - 是子进程先炸，还是父进程先炸

3. 再盯住读管道用的 `FILE *`，看它是不是从一开始就无效。

   ```bash
   gdb -q ./sperf \
     -ex 'break sperf.c:182' \
     -ex run \
     -ex next \
     -ex 'print in' \
     -ex 'print errno' \
     -ex 'call perror("fdopen")' \
     -ex quit \
     --args ./sperf ls
   ```

   如果这里已经不对，就不要继续看 `fgets` 后面的逻辑了。

4. 回头逐行核对文件描述符生命周期。重点只看这几行：`sperf/sperf.c:156`、`sperf/sperf.c:162`、`sperf/sperf.c:165`、`sperf/sperf.c:178`、`sperf/sperf.c:182`。

   在纸上画一张表就够了：

   - `pipe()` 之后，`pipefd[0]` / `pipefd[1]` 各是什么
   - `fork()` 之后，父子进程各自持有哪些 fd
   - 每次 `dup2()` 之后，哪个 fd 还是有效的
   - 每次 `close()` 之后，后面代码到底在用哪个 fd

5. 临时只加观测，不改设计。给这些调用都补返回值检查和 `perror`：

   - `pipe`
   - `fork`
   - `dup2`
   - `close`
   - `fdopen`

   这一轮的目标不是修，而是证明“哪一步以后状态变坏了”。

6. 再跑一遍编译器和 sanitizer，让工具替你报边界问题。

   ```bash
   cc -O0 -g -fsanitize=address,undefined -Wall -Wextra -std=gnu2x \
     sperf.c ../testkit/testkit.c -I../testkit -o sperf-asan
   ./sperf-asan ls
   ```

先证明“哪一步坏了”，再改代码。
