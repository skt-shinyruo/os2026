#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *defined_functions[100];
int defined_count;

int counter = 0;

// Compile a function definition and load it
bool compile_and_load_function(const char *function_def) {

    return false;
}

// Evaluate an expression
bool evaluate_expression(const char *expression, int *result) {
    // 生成一个唯一的 wrapper 名字，比如 __expr_wrapper_7
    int id = counter++;
    char wrapper_name[64];
    char wrapper_c_path[128];
    char wrapper_so_path[128];
    snprintf(wrapper_name, sizeof(wrapper_name), "__expr_wrapper_%d", id);
    snprintf(wrapper_c_path, sizeof(wrapper_c_path), "temp_expr_%d.c", id);
    snprintf(wrapper_so_path, sizeof(wrapper_so_path), "temp_expr_%d.so", id);

    // 生成临时 .c
    FILE *temp_c = tmpfile();
    temp_c = fopen(wrapper_c_path, "w");
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
