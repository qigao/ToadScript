/**
 * @file test_simple_opt.c
 * @brief Simple test for hash table and error handling optimizations
 */

#include <toad/engine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main() {
    printf("=== Simple Optimization Tests ===\n\n");

    // Test 1: Basic variable operations
    printf("Test 1: Basic variable operations...\n");
    {
        toad_env_t env;
        toad_env_init(&env);

        toad_env_set(&env, "x", toad_val_num(10));
        toad_env_set(&env, "y", toad_val_num(20));

        toad_value_t x = toad_env_get(&env, "x");
        toad_value_t y = toad_env_get(&env, "y");

        if (x.data.number == 10 && y.data.number == 20) {
            printf("  ✓ PASS\n");
        } else {
            printf("  ✗ FAIL\n");
        }

        toad_env_free(&env);
    }

    // Test 2: Division by zero
    printf("\nTest 2: Division by zero...\n");
    {
        toad_env_t env;
        toad_env_init(&env);

        const char *code = "1 / 0";
        toad_node_t *ast = toad_parse(code, strlen(code));
        if (ast) {
            toad_eval(ast, &env);
            if (env.flow == toad_FLOW_THROW) {
                printf("  ✓ PASS: Exception thrown\n");
                printf("  Message: %s\n", env.error_msg);
            } else {
                printf("  ✗ FAIL: No exception\n");
            }
            toad_free(ast);
        }

        toad_env_free(&env);
    }

    // Test 3: Array bounds
    printf("\nTest 3: Array out of bounds...\n");
    {
        toad_env_t env;
        toad_env_init(&env);

        const char *code = "arr = [1,2,3]; arr[10]";
        toad_node_t *ast = toad_parse(code, strlen(code));
        if (ast) {
            toad_eval(ast, &env);
            if (env.flow == toad_FLOW_THROW) {
                printf("  ✓ PASS: Exception thrown\n");
                printf("  Message: %s\n", env.error_msg);
            } else {
                printf("  ✗ FAIL: No exception\n");
            }
            toad_free(ast);
        }

        toad_env_free(&env);
    }

    // Test 4: Try/catch works
    printf("\nTest 4: Try/catch...\n");
    {
        toad_env_t env;
        toad_env_init(&env);

        const char *code = "result = 0; try { x = 1/0; } catch(e) { result = 42; }";
        toad_node_t *ast = toad_parse(code, strlen(code));
        if (ast) {
            toad_eval(ast, &env);
            toad_value_t result = toad_env_get(&env, "result");
            if (result.data.number == 42) {
                printf("  ✓ PASS: Caught exception\n");
            } else {
                printf("  ✗ FAIL: result = %g\n", result.data.number);
            }
            toad_free(ast);
        }

        toad_env_free(&env);
    }

    // Test 5: Performance test
    printf("\nTest 5: Hash table performance...\n");
    {
        toad_env_t env;
        toad_env_init(&env);

        // Insert 100 variables
        for (int i = 0; i < 100; i++) {
            char name[32];
            snprintf(name, sizeof(name), "var_%d", i);
            toad_env_set(&env, name, toad_val_num(i));
        }

        // Benchmark lookups
        clock_t start = clock();
        for (int iter = 0; iter < 1000; iter++) {
            for (int i = 0; i < 100; i++) {
                char name[32];
                snprintf(name, sizeof(name), "var_%d", i);
                toad_value_t val = toad_env_get(&env, name);
                (void)val;
            }
        }
        clock_t end = clock();

        double ms = (double)(end - start) / CLOCKS_PER_SEC * 1000;
        printf("  100,000 lookups: %.2f ms\n", ms);
        printf("  Average: %.3f μs per lookup\n", ms * 1000 / 100000);

        if (ms < 100) {
            printf("  ✓ PASS: Good performance\n");
        } else {
            printf("  ⚠ WARNING: Slower than expected\n");
        }

        toad_env_free(&env);
    }

    printf("\n=== All tests completed ===\n");
    return 0;
}
