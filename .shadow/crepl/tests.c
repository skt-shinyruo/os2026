#include <testkit.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>

// Feel free to rename them.
bool compile_and_load_function(const char* function_def);
bool evaluate_expression(const char* expression, int* result);

static bool dir_contains_suffix(const char *path, const char *suffix) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return false;
    }

    struct dirent *entry;
    size_t suffix_len = strlen(suffix);
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len >= suffix_len &&
            strcmp(entry->d_name + name_len - suffix_len, suffix) == 0) {
            found = true;
            break;
        }
    }

    closedir(dir);
    return found;
}

static bool dir_contains_fragment_and_suffix(const char *path,
                                             const char *fragment,
                                             const char *suffix) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return false;
    }

    struct dirent *entry;
    size_t suffix_len = strlen(suffix);
    bool found = false;

    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (strstr(entry->d_name, fragment) != NULL &&
            name_len >= suffix_len &&
            strcmp(entry->d_name + name_len - suffix_len, suffix) == 0) {
            found = true;
            break;
        }
    }

    closedir(dir);
    return found;
}

UnitTest(test_compile_valid_function) {
    bool result = compile_and_load_function("int test_func() { return 42; }");
    tk_assert(result == true, "Should successfully compile valid function");
}

UnitTest(test_compile_function_using_previous) {
    compile_and_load_function("int test_func() { return 42; }");

    bool result = compile_and_load_function("int test_func2() { return test_func(); }");
    tk_assert(result == true, "Should successfully compile function using previous function");
}

UnitTest(test_compile_invalid_syntax) {
    bool result = compile_and_load_function("int invalid_func() { return 42");
    tk_assert(result == false, "Should fail on syntax error");
}

UnitTest(test_evaluate_simple_constant) {
    int result_value;
    bool result = evaluate_expression("42", &result_value);
    tk_assert(result == true, "Should evaluate simple constant");
    tk_assert(result_value == 42, "Result should be 42");
}

UnitTest(test_evaluate_arithmetic) {
    int result_value;
    bool result = evaluate_expression("21 + 21", &result_value);
    tk_assert(result == true, "Should evaluate arithmetic expression");
    tk_assert(result_value == 42, "Result should be 42");
}

UnitTest(test_evaluate_function_call) {
    compile_and_load_function("int test_eval() { return 100; }");

    int result_value;
    bool result = evaluate_expression("test_eval()", &result_value);
    tk_assert(result == true, "Should evaluate function call");
    tk_assert(result_value == 100, "Result should be 100");
}

UnitTest(test_evaluate_complex_expression) {
    compile_and_load_function("int test_eval() { return 100; }");

    int result_value;
    bool result = evaluate_expression("test_eval() / 2", &result_value);
    tk_assert(result == true, "Should evaluate complex expression");
    tk_assert(result_value == 50, "Result should be 50");
}

UnitTest(test_evaluate_undefined_function) {
    int result_value;
    bool result = evaluate_expression("undefined_function()", &result_value);
    tk_assert(result == false, "Should fail on undefined function");
}

UnitTest(test_evaluate_syntax_error) {
    int result_value;
    bool result = evaluate_expression("21 +", &result_value);
    tk_assert(result == false, "Should fail on syntax error");
}

UnitTest(test_evaluate_temp_files_live_in_tmp_crepl) {
    int result_value;
    bool result = evaluate_expression("40 + 2", &result_value);

    tk_assert(result == true, "Should evaluate arithmetic expression");
    tk_assert(result_value == 42, "Result should be 42");
    tk_assert(access("/tmp/crepl", F_OK) == 0,
              "Temporary directory /tmp/crepl should exist");
    tk_assert(dir_contains_suffix("/tmp/crepl", ".c"),
              "Temporary C source should be created under /tmp/crepl");
    tk_assert(dir_contains_suffix("/tmp/crepl", ".so"),
              "Temporary shared object should be created under /tmp/crepl");
}

UnitTest(test_compile_temp_files_live_in_tmp_crepl) {
    bool result = compile_and_load_function("int temp_dir_func() { return 7; }");

    tk_assert(result == true, "Should compile a valid function");
    tk_assert(access("/tmp/crepl", F_OK) == 0,
              "Temporary directory /tmp/crepl should exist");
    tk_assert(dir_contains_fragment_and_suffix("/tmp/crepl", "temp_func_", ".c"),
              "Function source should be created under /tmp/crepl");
    tk_assert(dir_contains_fragment_and_suffix("/tmp/crepl", "temp_func_", ".so"),
              "Function shared object should be created under /tmp/crepl");
}
