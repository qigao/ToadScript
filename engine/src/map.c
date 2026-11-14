/**
 * @file map.c
 * @brief Salts hash map backed map for engine values.
 */

#include <toad/module.h>
#include <cstl.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  hash_map_t entries;
  mem_pool_t pool;
} toad_map_storage_t;

#define TOAD_MAP_ENTRY_LIMIT (SIZE_MAX / sizeof(toad_value_t))

static size_t toad_map_key_hash(const void *key, size_t key_size, void *ctx) {
  const char *text;
  size_t hash = 1469598103934665603ull;

  (void)key_size;
  (void)ctx;
  if (!key) return 0;
  text = *(char * const *)key;
  if (!text) return 0;
  while (*text) {
    hash ^= (unsigned char)*text++;
    hash *= 1099511628211ull;
  }
  return hash;
}

static bool toad_map_key_equal(const void *left, const void *right, size_t key_size,
                                 void *ctx) {
  const char *lhs;
  const char *rhs;

  (void)key_size;
  (void)ctx;
  if (!left || !right) return false;
  lhs = *(char * const *)left;
  rhs = *(char * const *)right;
  if (!lhs || !rhs) return lhs == rhs;
  return strcmp(lhs, rhs) == 0;
}

static toad_map_storage_t *toad_map_storage_create(void) {
  toad_map_storage_t *storage = (toad_map_storage_t *)calloc(1, sizeof(*storage));
  if (!storage) return NULL;
  if (mem_init(&storage->pool, 0) != 0) {
    free(storage);
    return NULL;
  }
  if (hash_map_init_bytes(&storage->entries,
                          sizeof(char *), _Alignof(char *),
                          sizeof(toad_value_t), _Alignof(toad_value_t),
                          TOAD_MAP_ENTRY_LIMIT,
                          toad_map_key_hash, toad_map_key_equal, NULL) != STL_OK) {
    mem_destroy(&storage->pool);
    free(storage);
    return NULL;
  }
  if (hash_map_reserve(&storage->entries, 8) != STL_OK) {
    hash_map_destroy(&storage->entries);
    mem_destroy(&storage->pool);
    free(storage);
    return NULL;
  }
  return storage;
}

static char **toad_map_find_key_slot(const toad_map_storage_t *storage, const char *key) {
  size_t capacity;

  if (!storage || !key) return NULL;
  capacity = hash_map_capacity(&storage->entries);
  for (size_t i = 0; i < capacity; ++i) {
    const void *slot = hash_map_key_at(&storage->entries, i);
    char *const *stored_key = (char *const *)slot;
    if (stored_key && *stored_key && strcmp(*stored_key, key) == 0)
      return (char **)stored_key;
  }
  return NULL;
}

toad_value_t toad_val_map(void) {
  toad_value_t val;
  memset(&val, 0, sizeof(val));
  val.type = TOAD_VAL_MAP;
  val.ownership = TOAD_VALUE_OWNED;
  val.data.map.htab = toad_map_storage_create();
  return val;
}

toad_value_t toad_val_object(void) {
  toad_value_t val = toad_val_map();
  val.type = TOAD_VAL_OBJECT;
  return val;
}

int toad_value_is_object_like(const toad_value_t *value) {
  return value && (value->type == TOAD_VAL_MAP || value->type == TOAD_VAL_OBJECT);
}

toad_value_t toad_map_get(const toad_value_t *map, const char *key) {
  char *lookup = (char *)key;
  toad_map_storage_t *storage;
  toad_value_t *value;

  if (!toad_value_is_object_like(map) || !map->data.map.htab || !key)
    return toad_val_num(0);
  storage = (toad_map_storage_t *)map->data.map.htab;
  value = (toad_value_t *)hash_map_get(&storage->entries, &lookup);
  return value ? toad_value_borrow(*value) : toad_val_num(0);
}

int toad_map_set(toad_value_t *map, const char *key, toad_value_t value) {
  char *lookup = (char *)key;
  char *owned_key;
  toad_map_storage_t *storage;
  toad_value_t value_copy;
  toad_value_t *slot;

  if (!toad_value_is_object_like(map) || !key)
    return -1;
  if (toad_value_is_object_like(&value) &&
      value.data.map.htab == map->data.map.htab)
    return -1;
  if (!map->data.map.htab) {
    map->data.map.htab = toad_map_storage_create();
    if (!map->data.map.htab) return -1;
  }

  storage = (toad_map_storage_t *)map->data.map.htab;
  if (toad_value_copy_to_pool(value, &storage->pool, &value_copy) != 0) return -1;
  slot = (toad_value_t *)hash_map_get(&storage->entries, &lookup);
  if (slot) {
    toad_value_destroy(slot);
    *slot = value_copy;
    return 0;
  }

  owned_key = mem_strdup(&storage->pool, key);
  if (!owned_key) {
    toad_value_destroy(&value_copy);
    return -1;
  }
  if (hash_map_put(&storage->entries, &owned_key, &value_copy) != STL_OK) {
    mem_free(&storage->pool, owned_key);
    toad_value_destroy(&value_copy);
    return -1;
  }
  return 0;
}

int toad_map_has(const toad_value_t *map, const char *key) {
  char *lookup = (char *)key;
  toad_map_storage_t *storage;

  if (!toad_value_is_object_like(map) || !map->data.map.htab || !key)
    return 0;
  storage = (toad_map_storage_t *)map->data.map.htab;
  return hash_map_contains(&storage->entries, &lookup) ? 1 : 0;
}

int toad_map_delete(toad_value_t *map, const char *key) {
  char *lookup = (char *)key;
  char **key_slot;
  char *owned_key;
  toad_map_storage_t *storage;
  toad_value_t removed;

  if (!toad_value_is_object_like(map) || !map->data.map.htab || !key)
    return 0;

  storage = (toad_map_storage_t *)map->data.map.htab;
  key_slot = toad_map_find_key_slot(storage, key);
  owned_key = key_slot ? *key_slot : NULL;
  if (hash_map_remove(&storage->entries, &lookup, &removed) != STL_OK)
    return 0;

  mem_free(&storage->pool, owned_key);
  toad_value_destroy(&removed);
  return 1;
}

size_t toad_map_count(const toad_value_t *map) {
  toad_map_storage_t *storage;

  if (!toad_value_is_object_like(map) || !map->data.map.htab)
    return 0;
  storage = (toad_map_storage_t *)map->data.map.htab;
  return hash_map_size(&storage->entries);
}

toad_value_t *toad_map_get_ptr(const toad_value_t *map, const char *key) {
  char *lookup = (char *)key;
  toad_map_storage_t *storage;

  if (!toad_value_is_object_like(map) || !map->data.map.htab || !key)
    return NULL;
  storage = (toad_map_storage_t *)map->data.map.htab;
  return (toad_value_t *)hash_map_get(&storage->entries, &lookup);
}

void toad_map_free(toad_value_t *map) {
  toad_map_storage_t *storage;
  size_t capacity;

  if (!toad_value_is_object_like(map) || !map->data.map.htab)
    return;

  storage = (toad_map_storage_t *)map->data.map.htab;
  capacity = hash_map_capacity(&storage->entries);
  for (size_t i = 0; i < capacity; ++i) {
    const void *key_slot = hash_map_key_at(&storage->entries, i);
    const void *value_slot = hash_map_value_at_const(&storage->entries, i);
    char *const *key = (char *const *)key_slot;
    const toad_value_t *value = (const toad_value_t *)value_slot;
    if (!key || !value) continue;
    mem_free(&storage->pool, *key);
    toad_value_destroy((toad_value_t *)value);
  }

  hash_map_destroy(&storage->entries);
  mem_destroy(&storage->pool);
  free(storage);
  map->data.map.htab = NULL;
}

toad_map_iter_t toad_map_iter_begin(const toad_value_t *map) {
  toad_map_iter_t it = {NULL, 0, 0};
  toad_map_storage_t *storage;

  if (!toad_value_is_object_like(map) || !map->data.map.htab)
    return it;
  storage = (toad_map_storage_t *)map->data.map.htab;
  it.htab = storage;
  it.pos = 0;
  it.bound = hash_map_capacity(&storage->entries);
  return it;
}

int toad_map_iter_next(toad_map_iter_t *it, const char **key, toad_value_t *value) {
  toad_map_storage_t *storage;

  if (!it || !it->htab)
    return 0;

  storage = (toad_map_storage_t *)it->htab;
  while (it->pos < it->bound) {
    size_t slot = it->pos++;
    const void *key_slot = hash_map_key_at(&storage->entries, slot);
    const void *value_slot = hash_map_value_at_const(&storage->entries, slot);
    char *const *stored_key = (char *const *)key_slot;
    const toad_value_t *stored_value = (const toad_value_t *)value_slot;
    if (!stored_key || !stored_value) continue;
    if (key) *key = *stored_key;
    if (value) *value = toad_value_borrow(*stored_value);
    return 1;
  }
  return 0;
}
