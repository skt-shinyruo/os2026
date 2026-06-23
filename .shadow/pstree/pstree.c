#include <ctype.h>
#include <dirent.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define COMM_LEN 256

typedef struct {
    bool show_pids;
    bool numeric_sort;
} Options;

typedef struct Process Process;

struct Process {
    pid_t pid;
    pid_t ppid;
    char comm[COMM_LEN];
    Process **children;
    size_t child_count;
    size_t child_capacity;
};

typedef struct StatInfo StatInfo;

struct StatInfo {
    pid_t pid;
    char comm[COMM_LEN];
    char state;
    pid_t ppid;
};

typedef struct {
    Process *items;
    size_t count;
    size_t capacity;
} ProcessList;

int parse_options(int argc, char *argv[], Options *options);
void print_version(void);
void print_usage(FILE *out, const char *program);
int collect_processes(ProcessList *processes);
int add_process(ProcessList *processes, StatInfo *statInfo);
Process *find_process(ProcessList *processes, pid_t pid);
int build_tree(ProcessList *processes);
void sort_children(ProcessList *processes);
Process *find_root(ProcessList *processes);
void print_tree(const Process *root, const Options *options, int depth);
void free_processes(ProcessList *processes);

static int read_comm(pid_t pid, char *buf, size_t n) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    buf[strcspn(buf, "\n")] = 0;
    fclose(f);
    return 0;
}

static int get_ppid_from_stat(pid_t pid, pid_t *ppid_out) {
    char path[64], line[4096];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    int id, ppid;
    char comm[256], state;
    if (sscanf(line, "%d (%255[^)]) %c %d", &id, comm, &state, &ppid) != 4)
        return -1;
    *ppid_out = (pid_t)ppid;
    return 0;
}

int parse_options(int argc, char *argv[], Options *options) {
    /* TODO: 实现命令行参数解析。 */

    struct option long_options[] = {{"show-pids", no_argument, NULL, 'p'},
                                    {"numeric-sort", no_argument, NULL, 'n'},
                                    {"version", no_argument, NULL, 'V'},
                                    {"help", no_argument, NULL, 'h'},
                                    {NULL, 0, NULL, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "pnVh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            options->show_pids = true;
            break;
        case 'n':
            options->numeric_sort = true;
            break;
        case 'V':
            print_version();
            exit(0);
        case 'h':
            print_usage(stdout, argv[0]);
            exit(0);
        default:
            print_usage(stderr, argv[0]);
            exit(1);
        }
    }
    return 0;
}

void print_version(void) {
    /* TODO: 打印版本信息。 */
    printf("pstree version: 12.88\n");
}

void print_usage(FILE *out, const char *program) {
    /* TODO: 打印用法或错误提示。 */
    fprintf(out, "Usage: %s [options]\n", program);
    fprintf(out, "Options:\n");
    fprintf(out, "  -p, --show-pids     Show PIDs\n");
    fprintf(out, "  -n, --numeric-sort  Sort by PID\n");
    fprintf(out, "  -V, --version       Show version\n");
    fprintf(out, "  -h, --help          Show this help\n");
}

int collect_processes(ProcessList *processes) {
    /* TODO: 从 /proc 收集所有进程。 */
    return 0;
}

int add_process(ProcessList *processes, StatInfo *statInfo) {
    /* TODO: 追加一个进程到 ProcessList。 */

    if (processes->count >= processes->capacity) {
        size_t new_capacity =
            (processes->capacity == 0) ? 16 : processes->capacity * 2;
        Process *new_items =
            realloc(processes->items, new_capacity * sizeof(Process));
        if (!new_items)
            return -1;
        processes->items = new_items;
        processes->capacity = new_capacity;
    }

    Process proc = {0};
    proc.pid = statInfo->pid;
    proc.ppid = statInfo->ppid;
    strncpy(proc.comm, statInfo->comm, COMM_LEN);
    proc.comm[COMM_LEN - 1] = 0;

    processes->items[processes->count++] = proc;

    return 0;
}

Process *find_process(ProcessList *processes, pid_t pid) {
    /* TODO: 按 pid 查找进程。 */
    for (size_t i = 0; i < processes->count; ++i) {
        if (processes->items[i].pid == pid) {
            return &processes->items[i];
        }
    }
    return NULL;
}

int build_tree(ProcessList *processes) {
    /* TODO: 根据 ppid 建立父子关系。 */
    for (size_t i = 0; i < processes->count; ++i) {
        Process *proc = &processes->items[i];
        Process *parent = find_process(processes, proc->ppid);
        if (!parent) {
            continue;
        }

        if (parent->child_count >= parent->child_capacity) {
            size_t new_capacity =
                (parent->child_capacity == 0) ? 16 : parent->child_capacity * 2;
            Process *new_children =
                realloc(parent->children, new_capacity * sizeof(Process));
            if (!new_children) {
                perror("children realloc");
                return -1;
            }
            parent->children = new_children;
            parent->child_capacity = new_capacity;
        }
        parent->children[parent->child_count++] = &proc;
    }
    return 0;
}

void sort_children(ProcessList *processes) {
    /* TODO: 对每个进程的 children 按 pid 排序。 */
    for (size_t i = 0; i < processes->count; ++i) {
        Process *proc = &processes->items[i];
        if (proc->child_count > 1) {
            qsort(proc->children, proc->child_count, sizeof(Process *),
                  [](const void *a, const void *b) {
                      const Process *pa = *(const Process **)a;
                      const Process *pb = *(const Process **)b;
                      return (pa->pid > pb->pid) - (pa->pid < pb->pid);
                  });
        }
    }
}

Process *find_root(ProcessList *processes) {
    /* TODO: 找到进程树的打印起点。 */
    return NULL;
}

void print_tree(const Process *root, const Options *options, int depth) {
    /* TODO: 递归打印进程树。 */
}

void free_processes(ProcessList *processes) {
    /* TODO: 释放进程表相关内存。 */
}

static int read_stat(pid_t pid, StatInfo *stat_info) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[4096];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    int id, ppid;
    char comm[256], state;
    if (sscanf(line, "%d (%255[^)]) %c %d", &id, comm, &state, &ppid) != 4)
        return -1;
    stat_info->pid = (pid_t)id;
    strncpy(stat_info->comm, comm, COMM_LEN);
    stat_info->comm[COMM_LEN - 1] = 0;
    stat_info->state = state;
    stat_info->ppid = (pid_t)ppid;
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /*
    pid_t self = getpid();
    pid_t parent = getppid();

    char self_comm[256] = "?", parent_comm[256] = "?";
    read_comm(self, self_comm, sizeof self_comm);
    read_comm(parent, parent_comm, sizeof parent_comm);

    printf("%s(%d)\n", parent_comm, parent);

    DIR *d = opendir("/proc");
    if (!d) {
        perror("opendir /proc");
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(de->d_name);

        pid_t ppid;
        if (get_ppid_from_stat(pid, &ppid) != 0)
            continue;
        if (ppid != parent)
            continue;

        char comm[256] = "?";
        read_comm(pid, comm, sizeof comm);

        printf("  |- %s(%d)%s\n", comm, pid, (pid == self) ? "  <== me" : "");
    }

    closedir(d);
    */

    Options options = {0};
    parse_options(argc, argv, &options);

    ProcessList processes = {0};

    DIR *d = opendir("/proc");
    if (!d) {
        perror("opendir /proc");
        return -1;
    }

    // 遍历
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0]))
            continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        pid_t ppid;
        if (get_ppid_from_stat(pid, &ppid) != 0)
            continue;
        char comm[COMM_LEN] = "?";

        StatInfo stat_info = {0};
        read_stat(pid, &stat_info);

        add_process(&processes, &stat_info);
    }

    build_tree(&processes);
    sort_children(&processes);

    printf("Collected %zu processes:\n", processes.count);
    for (size_t i = 0; i < processes.count; ++i) {
        Process *proc = &processes.items[i];
        printf("%s(%d) ppid=%d child_count=%zu child_capacity=%zu\n",
               proc->comm, proc->pid, proc->ppid, proc->child_count,
               proc->child_capacity);
        if (proc->child_count > 1) {
            printf("  Children: ");
            for (size_t j = 0; j < proc->child_count; ++j) {
                Process *child = proc->children[j];
                printf("%s(%d) ", child->comm, child->pid);
            }
            printf("\n");
        }

        closedir(d);

        return 0;
    }
