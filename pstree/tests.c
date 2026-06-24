#define _GNU_SOURCE
#include <testkit.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>

typedef struct Process Process;
struct Process {
    pid_t pid;
    pid_t ppid;
    char comm[256];
    Process **children;
    size_t child_count;
    size_t child_capacity;
};

typedef struct {
    bool show_pids;
    bool numeric_sort;
} Options;

typedef struct {
    Process *items;
    Process **by_pid;
    size_t count;
    size_t capacity;
} ProcessList;

void print_forest(const ProcessList *processes, const Options *options);
int build_pid_index(ProcessList *processes);
Process *find_process(const ProcessList *processes, pid_t pid);

static pid_t weird_name_pid;

static void start_weird_process(void) {
    weird_name_pid = fork();
    tk_assert(weird_name_pid >= 0, "fork should succeed");
    if (weird_name_pid == 0) {
        prctl(PR_SET_NAME, "x)y");
        for (;;) {
            pause();
        }
    }
    usleep(100000);
}

static void stop_weird_process(void) {
    if (weird_name_pid > 0) {
        kill(weird_name_pid, SIGKILL);
        waitpid(weird_name_pid, NULL, 0);
        weird_name_pid = -1;
    }
}

// ======================== Unit Tests ========================

UnitTest(print_forest_includes_all_roots) {
    Process items[3] = {0};
    Process *root_children[] = {&items[1]};
    char *buffer = NULL;
    size_t size = 0;
    FILE *captured = open_memstream(&buffer, &size);
    FILE *saved_stdout = stdout;

    items[0].pid = 100;
    items[0].ppid = 0;
    strcpy(items[0].comm, "root_a");
    items[0].children = root_children;
    items[0].child_count = 1;

    items[1].pid = 101;
    items[1].ppid = 100;
    strcpy(items[1].comm, "child_a");

    items[2].pid = 200;
    items[2].ppid = 0;
    strcpy(items[2].comm, "root_b");

    ProcessList processes = {.items = items, .count = 3, .capacity = 3};
    Options options = {0};

    tk_assert(captured != NULL, "open_memstream should succeed");
    stdout = captured;
    print_forest(&processes, &options);
    fflush(captured);
    stdout = saved_stdout;
    fclose(captured);

    tk_assert(strstr(buffer, "root_a") != NULL, "first root should print");
    tk_assert(strstr(buffer, "root_b") != NULL, "second root should print");
    free(buffer);
}

UnitTest(build_pid_index_supports_binary_lookup) {
    Process items[3] = {0};

    items[0].pid = 30;
    items[1].pid = 10;
    items[2].pid = 20;

    ProcessList processes = {
        .items = items,
        .count = 3,
        .capacity = 3,
    };

    tk_assert(build_pid_index(&processes) == 0,
              "building the pid index should succeed");

    tk_assert(processes.by_pid[0]->pid == 10, "lowest pid should come first");
    tk_assert(processes.by_pid[1]->pid == 20, "middle pid should come second");
    tk_assert(processes.by_pid[2]->pid == 30, "highest pid should come last");
    tk_assert(find_process(&processes, 20) == &items[2],
              "lookup should find the matching process");
}

// ======================== System Tests ========================

// Test the basic functionality without any arguments
SystemTest(basic_no_args, 
           ((const char *[]){"./pstree"})) {
    tk_assert(result->exit_status == 0, 
              "Basic pstree command should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Output should not be empty");
}

// Test the -p (--show-pids) option
SystemTest(show_pids_short, 
           ((const char *[]){"./pstree", "-p"})) {
    tk_assert(result->exit_status == 0, 
              "pstree -p should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Output should not be empty");
    // Check for presence of PIDs in output (numbers in parentheses)
    tk_assert(strstr(result->output, "(") != NULL, 
              "Output should contain PIDs in parentheses");
}

SystemTest(show_pids_long, 
           ((const char *[]){"./pstree", "--show-pids"})) {
    tk_assert(result->exit_status == 0, 
              "pstree --show-pids should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Output should not be empty");
    // Check for presence of PIDs in output (numbers in parentheses)
    tk_assert(strstr(result->output, "(") != NULL, 
              "Output should contain PIDs in parentheses");
}

// Test the -n (--numeric-sort) option
SystemTest(numeric_sort_short, 
           ((const char *[]){"./pstree", "-n"})) {
    tk_assert(result->exit_status == 0, 
              "pstree -n should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Output should not be empty");
    // Note: Testing the actual sorting would require parsing the output
}

SystemTest(numeric_sort_long, 
           ((const char *[]){"./pstree", "--numeric-sort"})) {
    tk_assert(result->exit_status == 0, 
              "pstree --numeric-sort should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Output should not be empty");
}

// Test the -V (--version) option
SystemTest(version_short, 
           ((const char *[]){"./pstree", "-V"})) {
    tk_assert(result->exit_status == 0, 
              "pstree -V should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Version information should not be empty");
    // Check for version-like information
    tk_assert(strstr(result->output, "pstree") != NULL,
              "Output should contain version information");
}

SystemTest(version_long, 
           ((const char *[]){"./pstree", "--version"})) {
    tk_assert(result->exit_status == 0, 
              "pstree --version should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Version information should not be empty");
    // Check for version-like information
    tk_assert(strstr(result->output, "pstree") != NULL,
              "Output should contain version information");
}

// Test combinations of options
SystemTest(show_pids_and_numeric_sort, 
           ((const char *[]){"./pstree", "-p", "-n"})) {
    tk_assert(result->exit_status == 0, 
              "pstree -p -n should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Output should not be empty");
    // Check for presence of PIDs in output
    tk_assert(strstr(result->output, "(") != NULL, 
              "Output should contain PIDs in parentheses");
}

SystemTest(all_options_long, 
           ((const char *[]){"./pstree", "--show-pids", "--numeric-sort"})) {
    tk_assert(result->exit_status == 0, 
              "pstree --show-pids --numeric-sort should exit with status 0, got %d", 
              result->exit_status);
    tk_assert(strlen(result->output) > 0, 
              "Output should not be empty");
    // Check for presence of PIDs in output
    tk_assert(strstr(result->output, "(") != NULL, 
              "Output should contain PIDs in parentheses");
}

SystemTest(invalid_option, 
           ((const char *[]){"./pstree", "--invalid-option"})) {
    // Program should exit with non-zero status for invalid options
    tk_assert(result->exit_status != 0, 
              "pstree with invalid option should exit with non-zero status");
    // Should print usage information
    tk_assert(strstr(result->output, "usage") != NULL || 
              strstr(result->output, "Usage") != NULL || 
              strstr(result->output, "invalid") != NULL || 
              strstr(result->output, "Invalid") != NULL,
              "Output should mention invalid option or show usage");
}

SystemTest(status_name_with_paren,
           ((const char *[]){"./pstree"}),
           .init = start_weird_process,
           .fini = stop_weird_process) {
    tk_assert(result->exit_status == 0,
              "pstree should exit cleanly, got %d", result->exit_status);
    tk_assert(strstr(result->output, "x)y") != NULL,
              "Output should contain the full process name");
}
