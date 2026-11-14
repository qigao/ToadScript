/**
 * @file class.h
 * @brief TurboScript OOP - Class and Instance Runtime Support
 *
 * This file defines the runtime structures for TurboScript's object-oriented
 * programming features, including:
 * - Class definitions with methods and constructors
 * - Instance objects with fields
 * - Prototype-based inheritance
 * - Method lookup and binding
 */

#ifndef TBS_CLASS_H
#define TBS_CLASS_H

#include <turboscript/types.h>
#include "memory/buffer.h"
#include <cmeta/cmeta.h>
#include <cmeta/data.h>
#include <cmeta/object.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct tbs_class_s tbs_class_t;
typedef struct tbs_instance_s tbs_instance_t;
typedef struct tbs_method_entry_s tbs_method_entry_t;

/*
 * Canonical CMeta storage descriptor for tbs_value_t.
 *
 * Stable identity: "turboscript.exprtk.value".
 * This is the single authority used by reflection, dynamic object projection,
 * and external CFlow carrier contracts such as TurboDB object-row output.
 */
TBS_C_API const cmeta_type_desc *tbs_value_cmeta_type(void);
TBS_C_API const cmeta_type_desc *tbs_value_ptr_cmeta_type(void);

/*
 * Canonical move/destroy lifecycle for an escaping tbs_value_t.
 *
 * This descriptor intentionally does not claim provider-neutral copy support:
 * copying dynamic values may require an explicit env/pool owner. It is suitable
 * for compiler/runtime ownership after a value has crossed an authoritative
 * promotion boundary such as tbs_call_builtin().
 */
TBS_C_API const cmeta_data_desc *tbs_value_cmeta_data(void);

/**
 * @brief Method entry - maps method name to function
 */
struct tbs_method_entry_s {
    char *name;
    tbs_func_t *func;
};

/**
 * @brief Class object - represents a class definition
 *
 * A class contains:
 * - Constructor function
 * - Instance methods (shared by all instances)
 * - Static methods (called on class, not instance)
 * - Prototype link (for inheritance)
 */
struct tbs_class_s {
    char *name;                      /* Class name */
    tbs_func_t *constructor;      /* Constructor function (optional) */
    void *constructors;              /* HTAB(tbs_method_entry_t)* - overloaded constructors */
    void *methods;                   /* HTAB(tbs_method_entry_t)* - instance methods */
    void *static_methods;            /* HTAB(tbs_method_entry_t)* - static methods */
    void *static_fields;             /* HTAB(field_entry_t)* - static fields */
    void *abstract_methods;          /* HTAB(tbs_method_entry_t)* - unresolved abstract methods */
    char **instance_field_names;      /* Canonical instance field layout for slot access */
    char **instance_field_types;      /* NULL for legacy dynamic fields */
    int *instance_field_access_levels;
    struct tbs_class_s **instance_field_owners;
    tbs_value_t *instance_field_defaults;
    unsigned char *instance_field_has_default;
    size_t instance_field_count;
    size_t instance_field_capacity;
    struct tbs_class_s **interfaces; /* Direct implemented/extended interfaces */
    size_t interface_count;
    size_t interface_capacity;
    struct tbs_class_s *prototype; /* Parent class (NULL for base class) */
    uint64_t type_id;                  /* Stable identity preserved across class clones */
    uint64_t static_field_version;    /* Bumped when the static field table shape changes */
    int is_abstract;                 /* Whether this class cannot be instantiated */
    int is_interface;                /* Whether this class is an interface constraint */
    int is_final;                    /* Whether this class cannot be extended */

    /* Canonical CMeta identity projected from the finalized script class shape.
     * The descriptor describes the native tbs_instance_t carrier while its
     * semantic identity is class-specific and deterministic across clones. */
    char *cmeta_stable_id;
    cmeta_type_identity cmeta_identity;
    cmeta_type_desc cmeta_type;
    int cmeta_ready;

    /* Canonical data/object surface. Logical fields are provider-backed and
     * always use CMETA_FIELD_DYNAMIC_OFFSET; no script slot is projected as a
     * native C struct offset. */
    cmeta_field_desc *cmeta_layout_fields;
    cmeta_data_field_desc *cmeta_data_fields;
    cmeta_struct_desc cmeta_struct;
    cmeta_data_struct_shape cmeta_shape;
    cmeta_data_desc cmeta_data;
    cmeta_object_field_provider cmeta_field_provider;
    int cmeta_data_ready;

    mem_pool_t *arena;               /* Memory arena for class metadata */
};

typedef struct tbs_cmeta_string_slot_s {
    char *data;
    size_t length;
    unsigned char owned;
} tbs_cmeta_string_slot_t;

/**
 * @brief Instance object - represents a class instance
 *
 * An instance contains:
 * - Reference to its class
 * - Instance fields (key-value pairs)
 */
struct tbs_instance_s {
    tbs_class_t *klass;  /* Pointer to class definition */
    void *fields;           /* HTAB(tbs_map_kv_t)* - instance fields (this.x, this.y, ...) */
    tbs_value_t *field_slots;      /* Field values indexed by klass->instance_field_names */
    unsigned char *field_slot_used;   /* Whether the corresponding slot has been assigned */
    bool *cmeta_bool_slots;            /* Exact _Bool projection for typed bool fields */
    tbs_cmeta_string_slot_t *cmeta_string_slots; /* Exact string projection for CMeta */
    size_t field_slot_count;
    size_t field_slot_capacity;
    uint64_t field_version;  /* Bumped when the instance field table shape changes */
    mem_pool_t *arena;      /* Memory arena for instance data */
};

/**
 * @brief Bound method - method with bound 'this' context
 *
 * When accessing a method on an instance (e.g., obj.method), we return
 * a bound method that captures both the function and the instance.
 */
typedef struct {
    tbs_instance_t *instance;  /* The 'this' object */
    tbs_func_t *method;        /* The method function */
} tbs_bound_method_t;

/* ========================================================================
 * Class Management API
 * ======================================================================== */

/**
 * @brief Create a new class
 * @param arena Memory arena for class metadata
 * @param name Class name (copied)
 * @param constructor_node Constructor AST node (optional, can be NULL)
 * @param method_nodes Array of method AST nodes
 * @param method_count Number of methods
 * @return New class object, or NULL on failure
 */
TBS_C_API tbs_class_t *tbs_class_create(
    mem_pool_t *arena,
    const char *name,
    tbs_node_t *constructor_node,
    tbs_node_t **method_nodes,
    size_t method_count
);

/**
 * @brief Clone class metadata into another arena
 * @param klass Source class
 * @param arena Destination arena
 * @return Class metadata owned by arena, or NULL on failure
 */
TBS_C_API tbs_class_t *tbs_class_clone_to_arena(
    tbs_class_t *klass,
    mem_pool_t *arena
);

/**
 * @brief Set parent class (for inheritance)
 * @param klass Child class
 * @param parent Parent class
 */
TBS_C_API void tbs_class_set_prototype(tbs_class_t *klass, tbs_class_t *parent);

/**
 * @brief Mark a class as explicitly abstract
 * @param klass The class
 * @param is_abstract Non-zero if abstract
 */
TBS_C_API void tbs_class_set_abstract(tbs_class_t *klass, int is_abstract);

TBS_C_API void tbs_class_set_interface(tbs_class_t *klass, int is_interface);

TBS_C_API void tbs_class_set_final(tbs_class_t *klass, int is_final);

TBS_C_API int tbs_class_is_final(tbs_class_t *klass);

TBS_C_API void tbs_class_add_interface(tbs_class_t *klass,
                                          tbs_class_t *interface_class);

TBS_C_API int tbs_class_is_a(tbs_class_t *klass, tbs_class_t *target);

/**
 * @brief Add a method to a class
 * @param klass The class
 * @param name Method name
 * @param method_func Method function
 * @param is_static Whether this is a static method
 */
TBS_C_API void tbs_class_add_method(
    tbs_class_t *klass,
    const char *name,
    tbs_func_t *method_func,
    int is_static
);

TBS_C_API void tbs_class_add_constructor(
    tbs_class_t *klass,
    tbs_func_t *constructor_func
);

TBS_C_API tbs_func_t *tbs_class_lookup_constructor_typed(
    tbs_class_t *klass,
    size_t argc,
    const tbs_value_t *args
);

/**
 * @brief Add an unresolved abstract method requirement to a class
 * @param klass The class
 * @param name Method name
 */
TBS_C_API void tbs_class_add_abstract_method(
    tbs_class_t *klass,
    const char *name
);

TBS_C_API void tbs_class_add_abstract_method_arity(
    tbs_class_t *klass,
    const char *name,
    size_t argc
);

TBS_C_API void tbs_class_add_abstract_method_signature(
    tbs_class_t *klass,
    const char *name,
    tbs_node_t **arg_params,
    size_t argc,
    tbs_env_t *closure_env,
    int is_static_method
);

TBS_C_API void tbs_class_add_abstract_methods_from(
    tbs_class_t *klass,
    tbs_class_t *interface_class
);

/**
 * @brief Lookup a method (searches prototype chain)
 * @param klass The class to start search from
 * @param name Method name
 * @param is_static Whether to search static methods
 * @return Method function, or NULL if not found
 */
TBS_C_API tbs_func_t *tbs_class_lookup_method(
    tbs_class_t *klass,
    const char *name,
    int is_static
);

TBS_C_API tbs_func_t *tbs_class_lookup_method_arity(
    tbs_class_t *klass,
    const char *name,
    int is_static,
    size_t argc
);

TBS_C_API tbs_func_t *tbs_class_lookup_method_signature(
    tbs_class_t *klass,
    tbs_func_t *signature
);

TBS_C_API tbs_func_t *tbs_class_lookup_method_typed(
    tbs_class_t *klass,
    const char *name,
    int is_static,
    size_t argc,
    const tbs_value_t *args
);

/**
 * @brief Get a static field value from a class (searches prototype chain)
 * @param klass The class
 * @param name Field name
 * @param out_value Output parameter for the value
 * @return 1 if field exists, 0 if not found
 */
TBS_C_API int tbs_class_get_static_field(
    tbs_class_t *klass,
    const char *name,
    tbs_value_t *out_value
);

TBS_C_API tbs_value_t *tbs_class_get_static_field_slot(
    tbs_class_t *klass,
    const char *name,
    tbs_class_t **owner_class
);

/**
 * @brief Set a static field value on a class
 * @param klass The class
 * @param name Field name (copied if new)
 * @param value Field value
 */
TBS_C_API void tbs_class_set_static_field(
    tbs_class_t *klass,
    const char *name,
    tbs_value_t value
);

TBS_C_API void tbs_class_declare_static_field(
    tbs_class_t *klass,
    const char *name,
    tbs_value_t value,
    int access_level
);

TBS_C_API int tbs_class_declare_static_field_typed(
    tbs_class_t *klass,
    const char *name,
    const char *declared_type,
    tbs_value_t value,
    int access_level
);

TBS_C_API const char *tbs_class_get_static_field_type(
    tbs_class_t *klass,
    const char *name
);

TBS_C_API int tbs_class_set_static_field_checked(
    tbs_class_t *klass,
    const char *name,
    tbs_value_t value,
    char *error_msg,
    size_t error_msg_len
);

TBS_C_API int tbs_class_get_static_field_access(
    tbs_class_t *klass,
    const char *name,
    tbs_class_t **owner_class
);

TBS_C_API int tbs_class_declare_instance_field(
    tbs_class_t *klass,
    const char *name,
    tbs_value_t default_value,
    int has_default,
    int access_level
);

TBS_C_API int tbs_class_declare_instance_field_typed(
    tbs_class_t *klass,
    const char *name,
    const char *declared_type,
    tbs_value_t default_value,
    int has_default,
    int access_level
);

TBS_C_API const char *tbs_class_get_instance_field_type(
    tbs_class_t *klass,
    const char *name
);

TBS_C_API int tbs_class_has_instance_field(
    tbs_class_t *klass,
    const char *name
);

TBS_C_API int tbs_class_get_instance_field_access(
    tbs_class_t *klass,
    const char *name,
    tbs_class_t **owner_class
);

/**
 * @brief Finalize abstract method requirements after methods/prototype are set
 * @param klass The class
 */
TBS_C_API void tbs_class_finalize_abstract_methods(tbs_class_t *klass);

/**
 * @brief Check whether a class is abstract
 * @param klass The class
 * @return Non-zero if the class cannot be instantiated
 */
TBS_C_API int tbs_class_is_abstract(tbs_class_t *klass);

TBS_C_API int tbs_class_is_interface(tbs_class_t *klass);

/**
 * @brief Finalize the canonical CMeta type identity for this class.
 *
 * The identity is derived from the finalized semantic shape (class name,
 * inheritance/interfaces and instance field name/type/access metadata), not
 * from the process-local type_id. Repeated finalization is allowed.
 */
TBS_C_API int tbs_class_finalize_cmeta(tbs_class_t *klass);

/**
 * @brief Return the finalized canonical CMeta type descriptor, or NULL.
 */
TBS_C_API const cmeta_type_desc *tbs_class_cmeta_type(
    const tbs_class_t *klass);

/**
 * @brief Finalize the canonical CMeta data/object surface for this class.
 *
 * Every reflected logical field is marked CMETA_FIELD_DYNAMIC_OFFSET and is
 * accessed only through the class-owned field provider. This never treats the
 * slot/hash-backed runtime as a fixed-layout C struct.
 */
TBS_C_API int tbs_class_finalize_cmeta_data(tbs_class_t *klass);

/** @brief Return the finalized canonical CMeta data descriptor, or NULL. */
TBS_C_API const cmeta_data_desc *tbs_class_cmeta_data(
    const tbs_class_t *klass);

/**
 * @brief Borrow one instance as a provider-backed canonical CMeta object.
 *
 * The borrow is valid while instance, its class metadata and field storage
 * remain alive. Mutation follows the existing TurboScript instance rules.
 */
TBS_C_API cmeta_status tbs_instance_borrow_cmeta_object(
    tbs_instance_t *instance, cmeta_object_ref *out);

/**
 * @brief Free class resources
 * @param klass Class to free
 */
TBS_C_API void tbs_class_destroy(tbs_class_t *klass);

/* ========================================================================
 * Instance Management API
 * ======================================================================== */

/**
 * @brief Create a new instance of a class
 * @param klass The class to instantiate
 * @param arena Memory arena for instance data
 * @return New instance object, or NULL on failure
 */
TBS_C_API tbs_instance_t *tbs_instance_create(
    tbs_class_t *klass,
    mem_pool_t *arena
);

/**
 * @brief Get a field value from an instance
 * @param instance The instance
 * @param name Field name
 * @param out_value Output parameter for the value
 * @return 1 if field exists, 0 if not found
 */
TBS_C_API int tbs_instance_get_field(
    tbs_instance_t *instance,
    const char *name,
    tbs_value_t *out_value
);

TBS_C_API tbs_value_t *tbs_instance_get_field_slot(
    tbs_instance_t *instance,
    const char *name
);

/**
 * @brief Set a field value on an instance
 * @param instance The instance
 * @param name Field name (copied if new)
 * @param value Field value
 */
TBS_C_API void tbs_instance_set_field(
    tbs_instance_t *instance,
    const char *name,
    tbs_value_t value
);

TBS_C_API int tbs_instance_set_field_checked(
    tbs_instance_t *instance,
    const char *name,
    tbs_value_t value,
    char *error_msg,
    size_t error_msg_len
);

typedef void (*tbs_instance_field_visitor_t)(tbs_value_t *value, void *user_data);
typedef void (*tbs_instance_named_field_visitor_t)(const char *name,
                                                       tbs_value_t *value,
                                                       void *user_data);

/**
 * @brief Visit all field values stored on an instance
 * @param instance The instance
 * @param visitor Callback invoked for each field value
 * @param user_data Opaque callback data
 */
TBS_C_API void tbs_instance_foreach_field(
    tbs_instance_t *instance,
    tbs_instance_field_visitor_t visitor,
    void *user_data
);

/**
 * @brief Visit all named field values stored on an instance
 * @param instance The instance
 * @param visitor Callback invoked for each field name and value
 * @param user_data Opaque callback data
 */
TBS_C_API void tbs_instance_foreach_named_field(
    tbs_instance_t *instance,
    tbs_instance_named_field_visitor_t visitor,
    void *user_data
);

/**
 * @brief Visit all static field values stored directly on a class
 * @param klass The class
 * @param visitor Callback invoked for each field value
 * @param user_data Opaque callback data
 */
TBS_C_API void tbs_class_foreach_static_field(
    tbs_class_t *klass,
    tbs_instance_field_visitor_t visitor,
    void *user_data
);

/**
 * @brief Visit all named static field values stored directly on a class
 * @param klass The class
 * @param visitor Callback invoked for each field name and value
 * @param user_data Opaque callback data
 */
TBS_C_API void tbs_class_foreach_named_static_field(
    tbs_class_t *klass,
    tbs_instance_named_field_visitor_t visitor,
    void *user_data
);

/**
 * @brief Get a method from an instance (searches class and prototype chain)
 * @param instance The instance
 * @param name Method name
 * @return Method function, or NULL if not found
 */
TBS_C_API tbs_func_t *tbs_instance_get_method(
    tbs_instance_t *instance,
    const char *name
);

TBS_C_API tbs_func_t *tbs_instance_get_method_arity(
    tbs_instance_t *instance,
    const char *name,
    size_t argc
);

/**
 * @brief Check if an instance is an instance of a class (including inheritance)
 * @param instance The instance
 * @param klass The class to check against
 * @return 1 if instance is of type klass, 0 otherwise
 */
TBS_C_API int tbs_instance_of(
    tbs_instance_t *instance,
    tbs_class_t *klass
);

/**
 * @brief Free instance resources
 * @param instance Instance to free
 */
TBS_C_API void tbs_instance_destroy(tbs_instance_t *instance);

/* ========================================================================
 * Value Constructors
 * ======================================================================== */

/**
 * @brief Create a class value
 * @param klass The class
 * @return tbs_value_t of type TBS_VAL_CLASS
 */
static inline tbs_value_t tbs_val_class(tbs_class_t *klass) {
    tbs_value_t v = {0};
    v.type = TBS_VAL_CLASS;
    v.data.class_val.klass = klass;
    return v;
}

/**
 * @brief Create an instance value
 * @param instance The instance
 * @return tbs_value_t of type TBS_VAL_INSTANCE
 */
static inline tbs_value_t tbs_val_instance(tbs_instance_t *instance) {
    tbs_value_t v = {0};
    v.type = TBS_VAL_INSTANCE;
    v.data.instance_val.instance = instance;
    return v;
}

/**
 * @brief Create a bound method value
 * @param instance The 'this' object
 * @param method The method function
 * @return tbs_value_t of type TBS_VAL_BOUND_METHOD
 */
static inline tbs_value_t tbs_val_bound_method(tbs_instance_t *instance,
                                                       tbs_func_t *method) {
    tbs_value_t v = {0};
    v.type = TBS_VAL_BOUND_METHOD;
    v.data.bound_method_val.instance = instance;
    v.data.bound_method_val.method = method;
    return v;
}

#ifdef __cplusplus
}
#endif

#endif /* TBS_CLASS_H */
