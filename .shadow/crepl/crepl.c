#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

char *defined_functions[100];
int defined_count;

int counter = 0;
static const char *temp_dir = "/tmp/crepl";

static bool ensure_temp_dir(void) {
    if (mkdir(temp_dir, 0755) == 0) {
        return true;
    }
    return errno == EEXIST;
}

static bool make_generated_paths(const char *prefix, int id, char *c_path,
                                 size_t c_path_size, char *so_path,
                                 size_t so_path_size) {
    int c_len =
        snprintf(c_path, c_path_size, "%s/%s_%d.c", temp_dir, prefix, id);
    int so_len =
        snprintf(so_path, so_path_size, "%s/%s_%d.so", temp_dir, prefix, id);

    if (c_len < 0 || c_len >= (int)c_path_size) {
        return false;
    }
    if (so_len < 0 || so_len >= (int)so_path_size) {
        return false;
    }
    return true;
}

static bool extract_function_name(const char *src, char *name, size_t size) {
    const char *p = src;
    size_t i = 0;

    while (isspace((unsigned char)*p))
        p++;

    if (strncmp(p, "int", 3) != 0) {
        return false;
    }
    p += 3;

    while (isspace((unsigned char)*p))
        p++;

    if (!(isalpha((unsigned char)*p) || *p == '_')) {
        return false;
    }

    while ((isalnum((unsigned char)*p) || *p == '_') && i + 1 < size) {
        name[i++] = *p++;
    }
    name[i] = '\0';

    while (isspace((unsigned char)*p))
        p++;

    return *p == '(';
}

// Compile a function definition and load it
bool compile_and_load_function(const char *function_def) {
    int id = counter++;
    char func_c_path[PATH_MAX];
    char func_so_path[PATH_MAX];

    if (!ensure_temp_dir()) {
        return false;
    }
    if (!make_generated_paths("temp_func", id, func_c_path, sizeof(func_c_path),
                              func_so_path, sizeof(func_so_path))) {
        return false;
    }

    FILE *temp_c = fopen(func_c_path, "w");
    if (temp_c == NULL) {
        return false;
    }

    fprintf(temp_c, "%s\n", function_def);
    fclose(temp_c);

    int pid = fork();
    if (pid == 0) {
        execlp("gcc", "gcc", "-shared", "-fPIC", func_c_path, "-o",
               func_so_path, NULL);
        exit(1);
    }

    int status;
    waitpid(pid, &status, 0);
    if (status != 0) {
        return false;
    }

    void *handle = dlopen(func_so_path, RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        return false;
    }
    return true;
}

// Evaluate an expression
bool evaluate_expression(const char *expression, int *result) {
    // 生成一个唯一的 wrapper 名字，比如 __expr_wrapper_7
    int id = counter++;
    char wrapper_name[64];
    char wrapper_c_path[PATH_MAX];
    char wrapper_so_path[PATH_MAX];
    snprintf(wrapper_name, sizeof(wrapper_name), "__expr_wrapper_%d", id);
    if (!ensure_temp_dir()) {
        return false;
    }
    if (!make_generated_paths("temp_expr", id, wrapper_c_path,
                              sizeof(wrapper_c_path), wrapper_so_path,
                              sizeof(wrapper_so_path))) {
        return false;
    }

    // 生成临时 .c
    FILE *temp_c = fopen(wrapper_c_path, "w");
    if (temp_c == NULL) {
        return false;
    }
    for (int i = 0; i < defined_count; i++) {
        fprintf(temp_c, "int %s();\n", defined_functions[i]);
    }
    fprintf(temp_c, "int %s() { return %s; }\n", wrapper_name, expression);
    fclose(temp_c);

    // compile c get so
    // fork and exec gcc to compile wrapper_name.c into wrapper_name.so
    int pid = fork();
    if (pid == 0) {
        // Child process
        execlp("gcc", "gcc", "-shared", "-fPIC", wrapper_c_path, "-o",
               wrapper_so_path, NULL);
        exit(1); // If exec fails
    }

    // Parent process
    int status;
    waitpid(pid, &status, 0);
    if (status != 0) {
        return false; // Compilation failed
    }

    // dlopen() 加载
    void *handle = dlopen(wrapper_so_path, RTLD_NOW | RTLD_GLOBAL);
    printf("handle: %p\n", handle);
    if (!handle) {
        return false;
    }

    // dlsym() 找到 __expr_wrapper_
    int (*wrapper_func)() = dlsym(handle, wrapper_name);
    if (!wrapper_func) {
        printf("dlsym failed: %s\n", dlerror());
        dlclose(handle);
        return false;
    }

    // 调这个函数
    *result = wrapper_func();

    // 把返回值写入 *result
    dlclose(handle);
    return true;
}

int main() {
    while (true) {
        char input[256];
        printf(
            "Enter a function definition or expression (or 'exit' to quit):\n");
        if (!fgets(input, sizeof(input), stdin)) {
            break; // EOF or error
        }

        // Remove newline character
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "exit") == 0) {
            break;
        }

        if (strstr(input, "int") != NULL) {
            // Assume it's a function definition
            if (compile_and_load_function(input)) {
                printf("Function compiled and loaded successfully.\n");
            } else {
                printf("Failed to compile function.\n");
            }
        } else {
            // Assume it's an expression
            int result;
            if (evaluate_expression(input, &result)) {
                printf("Result: %d\n", result);
            } else {
                printf("Failed to evaluate expression.\n");
            }
        }
    }
}
