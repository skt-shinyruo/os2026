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
    Process **by_pid;
    size_t count;
    size_t capacity;
} ProcessList;

int parse_options(int argc, char *argv[], Options *options);
void print_version(void);
void print_usage(FILE *out, const char *program);
int collect_processes(ProcessList *processes);
int add_process(ProcessList *processes, StatInfo *statInfo);
int build_pid_index(ProcessList *processes);
Process *find_process(const ProcessList *processes, pid_t pid);
int build_tree(ProcessList *processes);
void sort_children(ProcessList *processes);
void print_forest(const ProcessList *processes, const Options *options);
void print_tree(const Process *root, const Options *options, int depth);
void free_processes(ProcessList *processes);
static int read_status(pid_t pid, StatInfo *stat_info);

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
    DIR *d = opendir("/proc");
    if (!d) {
        perror("opendir /proc");
        return -1;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) {
            continue;
        }

        pid_t pid = (pid_t)atoi(de->d_name);
        StatInfo stat_info = {0};

        if (read_status(pid, &stat_info) < 0) {
            continue;
        }

        if (add_process(processes, &stat_info) < 0) {
            closedir(d);
            return -1;
        }
    }

    closedir(d);
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

int compare_children(const void *a, const void *b) {
    const Process *pa = *(const Process *const *)a;
    const Process *pb = *(const Process *const *)b;
    return (pa->pid > pb->pid) - (pa->pid < pb->pid);
}

int build_pid_index(ProcessList *processes) {
    Process **by_pid = realloc(processes->by_pid,
                               processes->count * sizeof(*processes->by_pid));
    if (!by_pid && processes->count > 0) {
        perror("realloc by_pid");
        return -1;
    }

    processes->by_pid = by_pid;
    for (size_t i = 0; i < processes->count; ++i) {
        processes->by_pid[i] = &processes->items[i];
    }

    qsort(processes->by_pid, processes->count, sizeof(*processes->by_pid),
          compare_children);
    return 0;
}

Process *find_process(const ProcessList *processes, pid_t pid) {
    /* TODO: 按 pid 查找进程。 */
    if (!processes->by_pid || processes->count == 0) {
        for (size_t i = 0; i < processes->count; ++i) {
            if (processes->items[i].pid == pid) {
                return &processes->items[i];
            }
        }
        return NULL;
    }

    Process key = {.pid = pid};
    Process *key_ptr = &key;
    Process **found =
        bsearch(&key_ptr, processes->by_pid, processes->count,
                sizeof(*processes->by_pid), compare_children);
    return found ? *found : NULL;
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
            Process **new_children = realloc(
                parent->children, new_capacity * sizeof(*parent->children));
            if (!new_children) {
                perror("children realloc");
                return -1;
            }
            parent->children = new_children;
            parent->child_capacity = new_capacity;
        }
        parent->children[parent->child_count++] = proc;
    }
    return 0;
}

void sort_children(ProcessList *processes) {
    /* TODO: 对每个进程的 children 按 pid 排序。 */
    for (size_t i = 0; i < processes->count; ++i) {
        Process *proc = &processes->items[i];
        if (proc->child_count > 1) {
            qsort(proc->children, proc->child_count, sizeof(Process *),
                  compare_children);
        }
    }
}

void print_forest(const ProcessList *processes, const Options *options) {
    Process **roots = malloc(processes->count * sizeof(*roots));
    if (!roots && processes->count > 0) {
        perror("malloc roots");
        return;
    }

    size_t root_count = 0;
    for (size_t i = 0; i < processes->count; ++i) {
        Process *proc = &processes->items[i];
        if (proc->ppid == 0 || find_process(processes, proc->ppid) == NULL) {
            roots[root_count++] = proc;
        }
    }

    if (options->numeric_sort && root_count > 1) {
        qsort(roots, root_count, sizeof(*roots), compare_children);
    }

    for (size_t i = 0; i < root_count; ++i) {
        print_tree(roots[i], options, 0);
    }

    free(roots);
}

void print_tree(const Process *root, const Options *options, int depth) {
    /* TODO: 递归打印进程树。 */
    if (options->show_pids) {
        printf("%*s%s(%d)\n", depth * 2, "", root->comm, root->pid);
    } else {
        printf("%*s%s\n", depth * 2, "", root->comm);
    }

    for (size_t i = 0; i < root->child_count; ++i) {
        print_tree(root->children[i], options, depth + 1);
    }
}

void free_processes(ProcessList *processes) {
    /* TODO: 释放进程表相关内存。 */
    for (size_t i = 0; i < processes->count; ++i) {
        free(processes->items[i].children);
    }
    free(processes->items);
    free(processes->by_pid);
    processes->items = NULL;
    processes->by_pid = NULL;
    processes->count = 0;
    processes->capacity = 0;
}

static int read_status(pid_t pid, StatInfo *stat_info) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    stat_info->pid = pid;
    stat_info->comm[0] = '\0';
    stat_info->state = '\0';
    stat_info->ppid = 0;

    bool have_name = false;
    bool have_ppid = false;
    char line[4096];

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Name:", 5) == 0) {
            char *value = line + 5;
            if (*value == '\t' || *value == ' ') {
                value++;
            }
            value[strcspn(value, "\n")] = '\0';
            strncpy(stat_info->comm, value, COMM_LEN);
            stat_info->comm[COMM_LEN - 1] = '\0';
            have_name = true;
        } else if (strncmp(line, "State:", 6) == 0) {
            char *value = line + 6;
            if (*value == '\t' || *value == ' ') {
                value++;
            }
            stat_info->state = *value;
        } else if (strncmp(line, "PPid:", 5) == 0) {
            char *value = line + 5;
            if (*value == '\t' || *value == ' ') {
                value++;
            }

            char *end;
            long ppid = strtol(value, &end, 10);
            if (end == value) {
                fclose(f);
                return -1;
            }

            stat_info->ppid = (pid_t)ppid;
            have_ppid = true;
        }
    }

    fclose(f);
    return (have_name && have_ppid) ? 0 : -1;
}

int main(int argc, char *argv[]) {
    Options options = {0};
    parse_options(argc, argv, &options);

    ProcessList processes = {0};

    if (collect_processes(&processes) < 0) {
        free_processes(&processes);
        return -1;
    }

    if (build_pid_index(&processes) < 0) {
        free_processes(&processes);
        return -1;
    }

    if (build_tree(&processes) < 0) {
        free_processes(&processes);
        return -1;
    }

    if (options.numeric_sort) {
        sort_children(&processes);
    }

    print_forest(&processes, &options);
    free_processes(&processes);
    return 0;
}
