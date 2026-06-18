# getopt_long 是什么？

C 标准库提供的**命令行参数解析函数**，自动帮你解析 `-m`、`--map` 这类选项。

## 你只需要做三件事

### 1. 包含头文件
```c
#include <getopt.h>
```

### 2. 定义两个东西

**短选项字符串**：告诉它有哪些短选项，冒号表示需要参数
```c
"m:p:"   // -m 和 -p 都需要参数
```

**长选项数组**：告诉它有哪些长选项
```c
struct option long_options[] = {
    {"map",     required_argument, NULL, 'm'},  // --map 需要参数，匹配时返回 'm'
    {"player",  required_argument, NULL, 'p'},  // --player 需要参数，匹配时返回 'p'
    {"move",    required_argument, NULL,  0 },  // --move 需要参数
    {"version", no_argument,       NULL, 'v'},  // --version 无参数，返回 'v'
    {0, 0, 0, 0}  // 结尾标志
};
```

### 3. 写一个循环解析

```c
int opt;
while ((opt = getopt_long(argc, argv, "m:p:", long_options, NULL)) != -1) {
    switch (opt) {
        case 'm':
            // optarg 就是文件名（getopt_long 自动帮你拿了）
            lab.map_file = optarg;
            break;
        case 'p':
            // 验证 player_id 是数字
            lab.player_id = optarg[0] - '0';
            break;
        case 'v':
            lab.show_version = 1;
            break;
        case '?':
            // 遇到未知选项或缺少参数
            return 1;
    }
}
```

## 它帮你自动处理了什么？

| 手写要做的 | getopt_long 自动帮你了 |
|---|---|
| `strcmp` 对比每个参数 | 自动匹配 `-m` 或 `--map` |
| 检查越界 `i+1 >= argc` | 自动检查，缺少参数返回 `?` |
| `argv[++i]` 取参数 | 通过 `optarg` 给你 |
| 未知选项提示 | 自动打印错误，返回 `?` |

## 在你的任务中对应关系

| 命令行输入 | 匹配后 opt 值 | optarg 值 |
|---|---|---|
| `-m map.txt` 或 `--map map.txt` | `'m'` | `"map.txt"` |
| `-p 3` 或 `--player 3` | `'p'` | `"3"` |
| `--move up` | 见下面说明 | `"up"` |
| `--version` | `'v'` | 无 |

关于 `--move`：因为短选项里没有对应的单字母，所以可以用一个不冲突的字符（比如 `0`）：
```c
{"move", required_argument, NULL, 0},  // 返回 0
```
然后在 switch 里加一个 `case 0:` 处理方向。

## 简单记忆：三步走

1. **配选项**：写好短选项字符串和长选项数组
2. **循环调用**：`getopt_long` 一次返回一个选项
3. **switch 处理**：每个匹配到的选项该存哪就存哪