/**
 * @file cmeta_object.c
 * @brief Canonical CMeta data/object projection for TurboScript classes.
 */
#include <turboscript/class.h>
#include <turboscript/module.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tbs_value_t is the exact storage carrier for dynamic/script-owned values.
 * Concrete scalar fields below project directly to their scalar member where
 * the native representation is exact. */
/* Stable identities are protocol metadata; source renaming must preserve them. */
static const cmeta_type_identity TBS_VALUE_CMETA_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("turboscript.exprtk.value");
static const cmeta_type_desc TBS_VALUE_CMETA_TYPE = {
    .name = "tbs_value_t",
    .size = sizeof(tbs_value_t),
    .align = _Alignof(tbs_value_t),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &TBS_VALUE_CMETA_IDENTITY
};

const cmeta_type_desc *tbs_value_cmeta_type(void) {
    return &TBS_VALUE_CMETA_TYPE;
}

static const cmeta_type_identity TBS_VALUE_PTR_CMETA_IDENTITY =
    CMETA_TYPE_ID_POINTER_INIT(&TBS_VALUE_CMETA_IDENTITY);
static const cmeta_type_desc TBS_VALUE_PTR_CMETA_TYPE = {
    .name = "tbs_value_t *",
    .size = sizeof(tbs_value_t *),
    .align = _Alignof(tbs_value_t *),
    .kind = CMETA_T_POINTER,
    .pointee = &TBS_VALUE_CMETA_TYPE,
    .traits = NULL,
    .identity = &TBS_VALUE_PTR_CMETA_IDENTITY
};

const cmeta_type_desc *tbs_value_ptr_cmeta_type(void) {
    return &TBS_VALUE_PTR_CMETA_TYPE;
}

static const char TBS_VALUE_CMETA_SHAPE[] =
    "turboscript.exprtk.value.lifecycle";

static cmeta_status tbs_value_cmeta_init_zero(void *object) {
    tbs_value_t *value = (tbs_value_t *)object;
    if (value == NULL) return CMETA_INVALID_ARGUMENT;
    memset(value, 0, sizeof(*value));
    value->type = TBS_VAL_NULL;
    return CMETA_OK;
}

static void tbs_value_cmeta_restore_zero(void *object) {
    tbs_value_t *value = (tbs_value_t *)object;
    if (value == NULL) return;
    tbs_value_destroy(value);
}

static void tbs_value_cmeta_move(void *destination, void *source) {
    tbs_value_t *to = (tbs_value_t *)destination;
    tbs_value_t *from = (tbs_value_t *)source;
    if (to == NULL || from == NULL || to == from) return;
    *to = *from;
    memset(from, 0, sizeof(*from));
    from->type = TBS_VAL_NULL;
}

static const cmeta_data_construct_ops TBS_VALUE_CMETA_CONSTRUCT_OPS = {
    .struct_size = sizeof(cmeta_data_construct_ops),
    .abi_version = CMETA_DATA_CONSTRUCT_OPS_ABI_VERSION,
    .storage_type = &TBS_VALUE_CMETA_TYPE,
    .init_zero = tbs_value_cmeta_init_zero,
    .restore_zero = tbs_value_cmeta_restore_zero,
    .move = tbs_value_cmeta_move
};

static const cmeta_data_desc TBS_VALUE_CMETA_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "turboscript.exprtk.value.data",
    .display_name = "TurboScript managed value",
    .kind = CMETA_DATA_CUSTOM,
    .storage_type = &TBS_VALUE_CMETA_TYPE,
    .shape = TBS_VALUE_CMETA_SHAPE,
    .buffer_ops = NULL,
    .enum_ops = NULL,
    .variant_ops = NULL,
    .fixed_ops = NULL,
    .enum_bits_ops = NULL,
    .collection_ops = NULL,
    .map_ops = NULL,
    .construct_ops = &TBS_VALUE_CMETA_CONSTRUCT_OPS
};

const cmeta_data_desc *tbs_value_cmeta_data(void) {
    return &TBS_VALUE_CMETA_DATA;
}

static const cmeta_type_identity TBS_STRING_SLOT_CMETA_IDENTITY =
    CMETA_TYPE_ID_ATOM_INIT("turboscript.exprtk.string-slot");
static const cmeta_type_desc TBS_STRING_SLOT_CMETA_TYPE = {
    .name = "tbs_cmeta_string_slot_t",
    .size = sizeof(tbs_cmeta_string_slot_t),
    .align = _Alignof(tbs_cmeta_string_slot_t),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = NULL,
    .identity = &TBS_STRING_SLOT_CMETA_IDENTITY
};

static char *tbs_cmeta_arena_join3(
    mem_pool_t *arena, const char *a, const char *b, const char *c) {
    size_t a_len;
    size_t b_len;
    size_t c_len;
    size_t total;
    char *out;

    if (!arena || !a || !b || !c) return NULL;
    a_len = strlen(a);
    b_len = strlen(b);
    c_len = strlen(c);
    if (a_len > SIZE_MAX - b_len ||
        a_len + b_len > SIZE_MAX - c_len ||
        a_len + b_len + c_len == SIZE_MAX)
        return NULL;
    total = a_len + b_len + c_len;
    out = (char *)mem_alloc(arena, total + 1u);
    if (!out) return NULL;
    memcpy(out, a, a_len);
    memcpy(out + a_len, b, b_len);
    memcpy(out + a_len + b_len, c, c_len);
    out[total] = '\0';
    return out;
}

static const char TBS_DYNAMIC_SHAPE[] = "turboscript.dynamic";
static const char TBS_DYNAMIC_LIST_SHAPE[] = "turboscript.dynamic.list";
static const char TBS_DYNAMIC_MAP_SHAPE[] = "turboscript.dynamic.map";
static const char TBS_DYNAMIC_INSTANCE_SHAPE[] = "turboscript.dynamic.instance";

#define TBS_DYNAMIC_DATA(name_, stable_, display_, shape_)                  \
    static const cmeta_data_desc name_ = {                                    \
        .struct_size = sizeof(cmeta_data_desc),                               \
        .abi_version = CMETA_DATA_DESC_ABI_VERSION,                           \
        .stable_id = stable_,                                                 \
        .display_name = display_,                                             \
        .kind = CMETA_DATA_CUSTOM,                                            \
        .storage_type = &TBS_VALUE_CMETA_TYPE,                             \
        .shape = shape_,                                                      \
        .buffer_ops = NULL,                                                   \
        .enum_ops = NULL,                                                     \
        .variant_ops = NULL,                                                  \
        .fixed_ops = NULL,                                                    \
        .enum_bits_ops = NULL,                                                \
        .collection_ops = NULL,                                               \
        .map_ops = NULL,                                                      \
        .construct_ops = NULL                                                 \
    }

TBS_DYNAMIC_DATA(TBS_DYNAMIC_DATA_VALUE,
                    "turboscript.exprtk.dynamic.data",
                    "TurboScript dynamic value", TBS_DYNAMIC_SHAPE);
TBS_DYNAMIC_DATA(TBS_DYNAMIC_LIST_DATA,
                    "turboscript.exprtk.dynamic-list.data",
                    "TurboScript dynamic list", TBS_DYNAMIC_LIST_SHAPE);
TBS_DYNAMIC_DATA(TBS_DYNAMIC_MAP_DATA,
                    "turboscript.exprtk.dynamic-map.data",
                    "TurboScript dynamic map/object", TBS_DYNAMIC_MAP_SHAPE);
TBS_DYNAMIC_DATA(TBS_DYNAMIC_INSTANCE_DATA,
                    "turboscript.exprtk.dynamic-instance.data",
                    "TurboScript dynamic instance", TBS_DYNAMIC_INSTANCE_SHAPE);

static bool tbs_string_cmeta_is_zero(const void *object) {
    const tbs_cmeta_string_slot_t *value =
        (const tbs_cmeta_string_slot_t *)object;
    return value != NULL && value->data == NULL &&
           value->length == 0u && value->owned == 0u;
}

static cmeta_status tbs_string_cmeta_init_zero(void *object) {
    tbs_cmeta_string_slot_t *value =
        (tbs_cmeta_string_slot_t *)object;
    if (value == NULL) return CMETA_INVALID_ARGUMENT;
    memset(value, 0, sizeof(*value));
    return CMETA_OK;
}

static cmeta_status tbs_string_cmeta_assign(
    void *object, const unsigned char *data, size_t size, size_t max_bytes) {
    tbs_cmeta_string_slot_t *value =
        (tbs_cmeta_string_slot_t *)object;
    char *copy;

    if (value == NULL || (size != 0u && data == NULL))
        return CMETA_INVALID_ARGUMENT;
    if (!tbs_string_cmeta_is_zero(value))
        return CMETA_INVALID_ARGUMENT;
    if (size > max_bytes || size == SIZE_MAX)
        return CMETA_CAPACITY_EXCEEDED;

    copy = (char *)malloc(size + 1u);
    if (!copy) return CMETA_OUT_OF_MEMORY;
    if (size != 0u) memcpy(copy, data, size);
    copy[size] = '\0';

    value->data = copy;
    value->length = size;
    value->owned = 1u;
    return CMETA_OK;
}

static void tbs_string_cmeta_restore_zero(void *object) {
    tbs_cmeta_string_slot_t *value =
        (tbs_cmeta_string_slot_t *)object;
    if (value == NULL) return;
    if (value->owned) free(value->data);
    memset(value, 0, sizeof(*value));
}

static cmeta_status tbs_string_cmeta_read(
    const void *object, const unsigned char **out_data, size_t *out_size) {
    const tbs_cmeta_string_slot_t *value =
        (const tbs_cmeta_string_slot_t *)object;
    if (value == NULL || out_data == NULL || out_size == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (value->length != 0u && value->data == NULL)
        return CMETA_CALLBACK_ERROR;
    *out_data = (const unsigned char *)value->data;
    *out_size = value->length;
    return CMETA_OK;
}

static void tbs_string_cmeta_move(void *destination, void *source) {
    tbs_cmeta_string_slot_t *to =
        (tbs_cmeta_string_slot_t *)destination;
    tbs_cmeta_string_slot_t *from =
        (tbs_cmeta_string_slot_t *)source;
    if (to == NULL || from == NULL) return;
    *to = *from;
    memset(from, 0, sizeof(*from));
}

static const cmeta_data_buffer_shape TBS_STRING_CMETA_SHAPE = {
    CMETA_DATA_BUFFER_OWNED
};
static const cmeta_data_buffer_ops TBS_STRING_CMETA_OPS = {
    .struct_size = sizeof(cmeta_data_buffer_ops),
    .abi_version = CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    .storage_type = &TBS_STRING_SLOT_CMETA_TYPE,
    .ownership = CMETA_DATA_BUFFER_OWNED,
    .is_zero = tbs_string_cmeta_is_zero,
    .assign = tbs_string_cmeta_assign,
    .restore_zero = tbs_string_cmeta_restore_zero,
    .read = tbs_string_cmeta_read,
    .init_zero = tbs_string_cmeta_init_zero,
    .move = tbs_string_cmeta_move
};
static const cmeta_data_desc TBS_STRING_CMETA_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "turboscript.exprtk.string.data",
    .display_name = "TurboScript string",
    .kind = CMETA_DATA_STRING,
    .storage_type = &TBS_STRING_SLOT_CMETA_TYPE,
    .shape = &TBS_STRING_CMETA_SHAPE,
    .buffer_ops = &TBS_STRING_CMETA_OPS,
    .enum_ops = NULL,
    .variant_ops = NULL,
    .fixed_ops = NULL,
    .enum_bits_ops = NULL,
    .collection_ops = NULL,
    .map_ops = NULL,
    .construct_ops = NULL
};

static const cmeta_data_desc *tbs_declared_field_data(const char *type) {
    if (type == NULL || type[0] == '\0')
        return &TBS_DYNAMIC_DATA_VALUE;
    if (strcmp(type, "int") == 0 || strcmp(type, "int64") == 0 ||
        strcmp(type, "integer") == 0)
        return &cmeta_data_int64;
    if (strcmp(type, "number") == 0 || strcmp(type, "float") == 0 ||
        strcmp(type, "double") == 0)
        return &cmeta_data_double;
    if (strcmp(type, "bool") == 0 || strcmp(type, "boolean") == 0)
        return &cmeta_data_bool;
    if (strcmp(type, "string") == 0)
        return &TBS_STRING_CMETA_DATA;
    if (strcmp(type, "list") == 0 || strcmp(type, "array") == 0 ||
        strcmp(type, "set") == 0)
        return &TBS_DYNAMIC_LIST_DATA;
    if (strcmp(type, "map") == 0 || strcmp(type, "object") == 0)
        return &TBS_DYNAMIC_MAP_DATA;
    return &TBS_DYNAMIC_INSTANCE_DATA;
}

static int tbs_cmeta_field_index(
    const tbs_class_t *klass, const cmeta_data_field_desc *field,
    size_t *out_index) {
    if (klass == NULL || field == NULL || out_index == NULL ||
        klass->cmeta_data_fields == NULL)
        return 0;
    if (field >= klass->cmeta_data_fields &&
        field < klass->cmeta_data_fields + klass->instance_field_count) {
        *out_index = (size_t)(field - klass->cmeta_data_fields);
        return 1;
    }
    for (size_t i = 0; i < klass->instance_field_count; ++i) {
        if (field->name != NULL && klass->instance_field_names[i] != NULL &&
            strcmp(field->name, klass->instance_field_names[i]) == 0) {
            *out_index = i;
            return 1;
        }
    }
    return 0;
}

static cmeta_status tbs_cmeta_field_read(
    void *context, const void *object, const cmeta_data_field_desc *field,
    const void **out_value) {
    const tbs_class_t *klass = (const tbs_class_t *)context;
    const tbs_instance_t *instance = (const tbs_instance_t *)object;
    const tbs_value_t *slot;
    size_t index = 0u;
    const cmeta_data_desc *data;

    if (out_value != NULL) *out_value = NULL;
    if (klass == NULL || instance == NULL || field == NULL ||
        out_value == NULL || instance->klass != klass ||
        !tbs_cmeta_field_index(klass, field, &index) ||
        index >= instance->field_slot_count ||
        instance->field_slot_used == NULL ||
        !instance->field_slot_used[index])
        return CMETA_TRAIT_MISSING;

    slot = &instance->field_slots[index];
    data = field->value;
    if (data == &cmeta_data_int64) {
        if (slot->type != TBS_VAL_INTEGER) return CMETA_TYPE_MISMATCH;
        *out_value = &slot->data.integer;
        return CMETA_OK;
    }
    if (data == &cmeta_data_double) {
        if (slot->type != TBS_VAL_NUMBER) return CMETA_TYPE_MISMATCH;
        *out_value = &slot->data.number;
        return CMETA_OK;
    }
    if (data == &cmeta_data_bool) {
        if (slot->type != TBS_VAL_BOOL || instance->cmeta_bool_slots == NULL)
            return CMETA_TYPE_MISMATCH;
        *out_value = &instance->cmeta_bool_slots[index];
        return CMETA_OK;
    }
    if (data == &TBS_STRING_CMETA_DATA) {
        tbs_cmeta_string_slot_t *projection;
        if (slot->type != TBS_VAL_STRING ||
            instance->cmeta_string_slots == NULL)
            return CMETA_TYPE_MISMATCH;
        projection = &((tbs_instance_t *)instance)->cmeta_string_slots[index];
        projection->data = (char *)slot->data.string.data;
        projection->length = slot->data.string.len;
        projection->owned = 0u;
        *out_value = projection;
        return CMETA_OK;
    }

    *out_value = slot;
    return CMETA_OK;
}

static cmeta_status tbs_cmeta_field_assign(
    void *context, void *object, const cmeta_data_field_desc *field,
    const void *value) {
    tbs_class_t *klass = (tbs_class_t *)context;
    tbs_instance_t *instance = (tbs_instance_t *)object;
    tbs_value_t converted;
    const cmeta_data_desc *data;
    size_t index = 0u;
    char error[256] = {0};

    if (klass == NULL || instance == NULL || field == NULL || value == NULL ||
        instance->klass != klass ||
        !tbs_cmeta_field_index(klass, field, &index))
        return CMETA_INVALID_ARGUMENT;

    data = field->value;
    if (data == &cmeta_data_int64)
        converted = tbs_val_int(*(const int64_t *)value);
    else if (data == &cmeta_data_double)
        converted = tbs_val_num(*(const double *)value);
    else if (data == &cmeta_data_bool)
        converted = tbs_val_bool(*(const bool *)value ? 1 : 0);
    else if (data == &TBS_STRING_CMETA_DATA) {
        const tbs_cmeta_string_slot_t *source =
            (const tbs_cmeta_string_slot_t *)value;
        if (source->length != 0u && source->data == NULL)
            return CMETA_TYPE_MISMATCH;
        converted = tbs_val_str(
            vstr_from_buf(source->data ? source->data : "", source->length));
    } else {
        const tbs_value_t *source = (const tbs_value_t *)value;
        converted = tbs_value_borrow(*source);
    }

    if (!tbs_instance_set_field_checked(
            instance, field->name, converted, error, sizeof(error)))
        return CMETA_TYPE_MISMATCH;
    return CMETA_OK;
}

int tbs_class_finalize_cmeta_data(tbs_class_t *klass) {
    char *data_id;

    if (klass == NULL || klass->arena == NULL) return 0;
    if (klass->cmeta_data_ready == 1) return 1;
    if (!tbs_class_finalize_cmeta(klass)) return 0;

    if (klass->instance_field_count != 0u) {
        klass->cmeta_layout_fields = (cmeta_field_desc *)mem_alloc_array(
            klass->arena, sizeof(cmeta_field_desc), klass->instance_field_count);
        klass->cmeta_data_fields = (cmeta_data_field_desc *)mem_alloc_array(
            klass->arena, sizeof(cmeta_data_field_desc), klass->instance_field_count);
        if (klass->cmeta_layout_fields == NULL || klass->cmeta_data_fields == NULL)
            goto fail;
        memset(klass->cmeta_layout_fields, 0,
               sizeof(cmeta_field_desc) * klass->instance_field_count);
        memset(klass->cmeta_data_fields, 0,
               sizeof(cmeta_data_field_desc) * klass->instance_field_count);
    }

    for (size_t i = 0; i < klass->instance_field_count; ++i) {
        const char *name = klass->instance_field_names[i];
        const char *declared = klass->instance_field_types
                                   ? klass->instance_field_types[i]
                                   : NULL;
        const cmeta_data_desc *data = tbs_declared_field_data(declared);
        char *field_id;

        if (name == NULL || !cmeta_data_desc_valid(data) ||
            data->storage_type == NULL ||
            !cmeta_type_desc_valid(data->storage_type))
            goto fail;

        field_id = tbs_cmeta_arena_join3(
            klass->arena, klass->cmeta_stable_id, ".field.", name);
        if (field_id == NULL) goto fail;

        klass->cmeta_layout_fields[i] = (cmeta_field_desc){
            .name = name,
            .type_name = declared != NULL ? declared : "dynamic",
            .offset = CMETA_FIELD_DYNAMIC_OFFSET,
            .size = data->storage_type->size,
            .align = data->storage_type->align,
            .type = data->storage_type,
            .declared_type = NULL
        };
        klass->cmeta_data_fields[i] = (cmeta_data_field_desc){
            .stable_id = field_id,
            .name = name,
            .offset = CMETA_FIELD_DYNAMIC_OFFSET,
            .value = data
        };
    }

    klass->cmeta_struct = (cmeta_struct_desc){
        .name = klass->name,
        .size = sizeof(tbs_instance_t),
        .align = _Alignof(tbs_instance_t),
        .fields = klass->cmeta_layout_fields,
        .field_count = klass->instance_field_count
    };
    klass->cmeta_shape = (cmeta_data_struct_shape){
        .layout = &klass->cmeta_struct,
        .fields = klass->cmeta_data_fields,
        .field_count = klass->instance_field_count
    };
    data_id = tbs_cmeta_arena_join3(
        klass->arena, klass->cmeta_stable_id, ".data", "");
    if (data_id == NULL) goto fail;

    klass->cmeta_data = (cmeta_data_desc){
        .struct_size = sizeof(cmeta_data_desc),
        .abi_version = CMETA_DATA_DESC_ABI_VERSION,
        .stable_id = data_id,
        .display_name = klass->name,
        .kind = CMETA_DATA_STRUCT,
        .storage_type = &klass->cmeta_type,
        .shape = &klass->cmeta_shape,
        .buffer_ops = NULL,
        .enum_ops = NULL,
        .variant_ops = NULL,
        .fixed_ops = NULL,
        .enum_bits_ops = NULL,
        .collection_ops = NULL,
        .map_ops = NULL,
        .construct_ops = NULL
    };
    klass->cmeta_field_provider = (cmeta_object_field_provider){
        .size = sizeof(cmeta_object_field_provider),
        .data = &klass->cmeta_data,
        .context = klass,
        .assign = tbs_cmeta_field_assign,
        .read = tbs_cmeta_field_read
    };

    if (!cmeta_data_desc_valid(&klass->cmeta_data) ||
        !cmeta_object_field_provider_valid(&klass->cmeta_field_provider))
        goto fail;

    klass->cmeta_data_ready = 1;
    return 1;

fail:
    klass->cmeta_data_ready = 0;
    return 0;
}

const cmeta_data_desc *tbs_class_cmeta_data(const tbs_class_t *klass) {
    return klass != NULL && klass->cmeta_data_ready == 1
               ? &klass->cmeta_data
               : NULL;
}

cmeta_status tbs_instance_borrow_cmeta_object(
    tbs_instance_t *instance, cmeta_object_ref *out) {
    tbs_class_t *klass;
    if (instance == NULL || out == NULL || instance->klass == NULL)
        return CMETA_INVALID_ARGUMENT;
    klass = instance->klass;
    if (!tbs_class_finalize_cmeta_data(klass))
        return CMETA_CALLBACK_ERROR;
    return cmeta_object_borrow_with_providers(
        out, instance, &klass->cmeta_data,
        &klass->cmeta_field_provider, NULL);
}
