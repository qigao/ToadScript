#include "fuzzy.h"
#include "tinytest.h"

#include <string.h>

static toad_builtin_fn find_function(const toad_module_t *mod, const char *name) {
    for (size_t i = 0; i < mod->count; ++i) {
        if (strcmp(mod->entries[i].name, name) == 0)
            return mod->entries[i].fn;
    }
    return NULL;
}

static toad_value_t str_arg(const char *s) {
    return toad_val_str(vstr_from_buf((char *)s, strlen(s)));
}

static toad_value_t map_get(toad_value_t *map, const char *key) {
    return toad_map_get(map, key);
}

spec("fuzzy_module") {
    describe("regex") {
        it("should return byte and utf8 offsets for utf8 text") {
            mem_pool_t arena;
            toad_builtin_fn fn;
            toad_value_t args[2];
            toad_value_t result;
            toad_value_t matched;
            toad_value_t start;
            toad_value_t utf8_start;
            toad_value_t text;

            mem_init(&arena, 4096);
            fn = find_function(toad_module_fuzzy(), "regex");
            check_not_null(fn);

            args[0] = str_arg("caf.");
            args[1] = str_arg("xx caf\xC3\xA9 yy");
            result = fn(2, args, NULL, &arena);

            check((result.type) == (TOAD_VAL_MAP));
            matched = map_get(&result, "matched");
            start = map_get(&result, "start");
            utf8_start = map_get(&result, "utf8_start");
            text = map_get(&result, "text");
            check((matched.type) == (TOAD_VAL_NUMBER));
            check(fabs((double)(matched.data.number) - (double)(1.0)) <= (double)(0.001));
            check((start.type) == (TOAD_VAL_INTEGER));
            check(((int)start.data.integer) == (3));
            check((utf8_start.type) == (TOAD_VAL_INTEGER));
            check(((int)utf8_start.data.integer) == (3));
            check((text.type) == (TOAD_VAL_STRING));
            check(strcmp((text.data.string.data), ("caf\xC3\xA9")) == 0);

            mem_destroy(&arena);
        }
    }

    describe("search") {
        it("should match utf8 text with substitution cost") {
            mem_pool_t arena;
            toad_builtin_fn fn;
            toad_value_t args[3];
            toad_value_t result;
            toad_value_t matched;
            toad_value_t cost;
            toad_value_t text;

            mem_init(&arena, 4096);
            fn = find_function(toad_module_fuzzy(), "search");
            check_not_null(fn);

            args[0] = str_arg("cafe");
            args[1] = str_arg("xx caf\xC3\xA9 yy");
            args[2] = toad_val_int(1);
            result = fn(3, args, NULL, &arena);

            check((result.type) == (TOAD_VAL_MAP));
            matched = map_get(&result, "matched");
            cost = map_get(&result, "cost");
            text = map_get(&result, "text");
            check(fabs((double)(matched.data.number) - (double)(1.0)) <= (double)(0.001));
            check((cost.type) == (TOAD_VAL_INTEGER));
            check(((int)cost.data.integer) == (1));
            check((text.type) == (TOAD_VAL_STRING));
            check(strcmp((text.data.string.data), ("caf\xC3\xA9")) == 0);

            mem_destroy(&arena);
        }

        it("should treat metacharacters literally by default") {
            mem_pool_t arena;
            toad_builtin_fn fn;
            toad_value_t args[3];
            toad_value_t result;
            toad_value_t text;

            mem_init(&arena, 4096);
            fn = find_function(toad_module_fuzzy(), "search");
            check_not_null(fn);

            args[0] = str_arg("a.c");
            args[1] = str_arg("xx a.c yy");
            args[2] = toad_val_int(0);
            result = fn(3, args, NULL, &arena);

            text = map_get(&result, "text");
            check((text.type) == (TOAD_VAL_STRING));
            check(strcmp((text.data.string.data), ("a.c")) == 0);

            mem_destroy(&arena);
        }
    }

    describe("fuzzy_regex_search") {
        it("should allow approximate regex matching") {
            mem_pool_t arena;
            toad_builtin_fn fn;
            toad_value_t args[3];
            toad_value_t result;
            toad_value_t matched;

            mem_init(&arena, 4096);
            fn = find_function(toad_module_fuzzy(), "fuzzy_regex_search");
            check_not_null(fn);

            args[0] = str_arg("br.ve");
            args[1] = str_arg("hello brave world");
            args[2] = toad_val_int(0);
            result = fn(3, args, NULL, &arena);

            matched = map_get(&result, "matched");
            check(fabs((double)(matched.data.number) - (double)(1.0)) <= (double)(0.001));

            mem_destroy(&arena);
        }
    }

    describe("levenshtein") {
        it("should return utf8 approximate matches") {
            mem_pool_t arena;
            toad_builtin_fn fn;
            toad_value_t args[3];
            toad_value_t result;
            int found_full = 0;

            mem_init(&arena, 4096);
            fn = find_function(toad_module_fuzzy(), "levenshtein");
            check_not_null(fn);

            args[0] = str_arg("caf\xC3\xA9");
            args[1] = str_arg("xx cafe yy");
            args[2] = toad_val_int(1);
            result = fn(3, args, NULL, &arena);

            check((result.type) == (TOAD_VAL_LIST));
            check(result.data.list.count > 0);
            for (size_t i = 0; i < result.data.list.count; ++i) {
                toad_value_t item = result.data.list.items[i];
                toad_value_t distance = map_get(&item, "distance");
                toad_value_t text = map_get(&item, "text");
                if (distance.type == TOAD_VAL_INTEGER && distance.data.integer == 1 &&
                    text.type == TOAD_VAL_STRING && strcmp(text.data.string.data, "cafe") == 0) {
                    found_full = 1;
                    break;
                }
            }
            check(found_full);

            mem_destroy(&arena);
        }
    }

    describe("ac") {
        it("should return multi-pattern utf8 matches") {
            mem_pool_t arena;
            toad_builtin_fn fn;
            toad_value_t patterns;
            toad_value_t args[2];
            toad_value_t result;
            toad_value_t item;
            toad_value_t pattern_id;
            toad_value_t text;

            mem_init(&arena, 4096);
            fn = find_function(toad_module_fuzzy(), "ac");
            check_not_null(fn);

            patterns = toad_val_list_empty();
            toad_list_push(&patterns, str_arg("caf\xC3\xA9"));
            toad_list_push(&patterns, str_arg("tea"));
            args[0] = patterns;
            args[1] = str_arg("xx caf\xC3\xA9 tea");
            result = fn(2, args, NULL, &arena);

            check((result.type) == (TOAD_VAL_LIST));
            check(result.data.list.count >= 2);
            item = result.data.list.items[0];
            pattern_id = map_get(&item, "pattern_id");
            text = map_get(&item, "text");
            check((pattern_id.type) == (TOAD_VAL_INTEGER));
            check(((int)pattern_id.data.integer) == (0));
            check((text.type) == (TOAD_VAL_STRING));
            check(strcmp((text.data.string.data), ("caf\xC3\xA9")) == 0);

            mem_destroy(&arena);
        }
    }
}
