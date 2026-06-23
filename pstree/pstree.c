#include <ctype.h>
#include <dirent.h>
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

typedef struct {
    Process *items;
    size_t count;
    size_t capacity;
} ProcessList;

int parse_options(int argc, char *argv[], Options *options);
void print_version(void);
void print_usage(FILE *out, const char *program);
int collect_processes(ProcessList *processes);
int add_process(ProcessList *processes, pid_t pid, pid_t ppid,
                const char *comm);
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
    return 0;
}

void print_version(void) {
    /* TODO: 打印版本信息。 */
}

void print_usage(FILE *out, const char *program) {
    /* TODO: 打印用法或错误提示。 */
}

int collect_processes(ProcessList *processes) {
    /* TODO: 从 /proc 收集所有进程。 */
    return 0;
}

int add_process(ProcessList *processes, pid_t pid, pid_t ppid,
                const char *comm) {
    /* TODO: 追加一个进程到 ProcessList。 */
    return 0;
}

Process *find_process(ProcessList *processes, pid_t pid) {
    /* TODO: 按 pid 查找进程。 */
    return NULL;
}

int build_tree(ProcessList *processes) {
    /* TODO: 根据 ppid 建立父子关系。 */
    return 0;
}

void sort_children(ProcessList *processes) {
    /* TODO: 对每个进程的 children 按 pid 排序。 */
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

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

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
    return 0;
}
