  先把目标缩小：你不是在“写一个游戏”，而是在写一个命令行工具。它每次运行只做一件事：读地图文件、检查是否合法、
  打印地图或移动玩家、用返回值告诉调用者成功还是失败。

  你现在从这里开始：

  cd /home/feng/code/os/jyy/os2026/labyrinth
  make all
  TK_VERBOSE=1 ./labyrinth

  第一次测试大概率会失败，这是正常的。你要把 labyrinth/tests.c 当成“任务清单”，把 labyrinth/labyrinth.c 里的
  TODO 一个个补起来。

  建议顺序：

  1. 先读懂数据结构：看 labyrinth/labyrinth.h，确认 Labyrinth 保存地图、行数、列数，Position 保存坐标。
  2. 先做最小函数：isValidPlayer、isEmptySpace、findPlayer、findFirstEmptySpace。这些不用管命令行，最适合入门。
  3. 每做完一个函数就跑测试，不要一次写很多。用 TK_VERBOSE=1 ./labyrinth 看失败原因。
  4. 再做地图读取：loadMap。你要检查文件能否打开、每行长度是否一致、字符是否合法、行列是否超过 100。
  5. 再做连通性：isConnected 和 dfs。把 . 和数字玩家都看成可走格子，用 DFS/BFS 从第一个可走格子出发，看能否访问
     所有可走格子。

  6. 再做移动：movePlayer。先找到玩家；如果玩家不存在，从第一个空地生成；然后根据方向算目标格；只有目标是 . 才
     能移动；失败时不要改地图。

  7. 最后做 main：解析 --version、--map/-m、--player/-p、--move，把前面写好的函数串起来。这里建议先画出每种参数
     组合的流程，不要直接硬写。

  你每一步都问自己三个问题：

  - 这个函数的输入是什么？
  - 成功和失败分别返回什么？
  - 失败时地图文件或内存里的地图是否应该保持不变？

  最重要的学习方法：不要从 main 开始写。先让单元测试过，再让系统测试过。等小函数都可靠了，main
  只是把它们按实验页面要求拼起来。实验要求来源：https://jyywiki.cn/OS/2026/labs/M1.md
  (https://jyywiki.cn/OS/2026/labs/M1.md)。



推荐流程：

  cd /home/feng/code/os/jyy/os2026/labyrinth
  make clean
  make all
  TK_RUN=1 ./labyrinth

  如果有失败，想看更详细输出：

  TK_VERBOSE=1 ./labyrinth

  看到类似这样才算本地测试通过：

  TestKit
  - [PASS] test_version (...)
  - [PASS] ...
  - 18/18 test cases passed.

  你也应该手动测几个命令，尤其看退出码：

  ./labyrinth --version
  echo $?

  ./labyrinth -m maps/map.txt -p 1
  echo $?

  ./labyrinth -m maps/map.txt -p 1 --move right
  echo $?

  规则是：成功返回 0，失败返回非 0，这个实验一般要求失败返回 1。

  注意两点：

  1. testkit 是本地检查，不等于最终 Online Judge。它能帮你发现大部分明显问题，但不保证覆盖所有边界
     情况。

  2. 如果 make all 还报 ../oslab.mk 缺失，先按前面说的恢复根目录 oslab.
  

 labyrinth/Makefile 里有两行：

  include ../oslab.mk
  include ../.shadow/oslab.mk

  你当前在 labyrinth/ 目录里运行 make all，所以 ../oslab.mk 应该指向仓库根目录的 oslab.mk。但你现在
  仓库根目录没有这个文件，因此 make 在读 Makefile 时就停止了。

  我查到当前仓库里：

  - 有 ../.shadow/oslab.mk
  - 没有 ../oslab.mk
  - main 分支里有 oslab.mk
  - 你现在在 M1_Start 分支

  你可以在仓库根目录执行：

  git restore --source=main oslab.mk

  然后再运行：

  cd labyrinth
  make all

  如果你的 Git 版本不支持 restore，可以用：

  git show main:oslab.mk > oslab.mk