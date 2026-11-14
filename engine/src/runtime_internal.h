#ifndef TOAD_RUNTIME_INTERNAL_H
#define TOAD_RUNTIME_INTERNAL_H

#include <toad/engine.h>

typedef enum toad_registration_status_e {
    TOAD_REGISTRATION_OK = 0,
    TOAD_REGISTRATION_INVALID_ARGUMENT = 1,
    TOAD_REGISTRATION_CONFLICT = 2,
    TOAD_REGISTRATION_OUT_OF_MEMORY = 3,
} toad_registration_status_t;

typedef struct toad_native_registration_s {
    const char *name;
    toad_native_fn fn;
    void *user_data;
    toad_native_flags_t flags;
} toad_native_registration_t;

typedef int (*toad_registration_fault_fn)(size_t allocation_index,
                                            void *user_data);

toad_registration_status_t toad_env_register_funcs_checked(
    toad_env_t *env, const toad_native_registration_t *registrations,
    size_t registration_count);

/* Internal deterministic fault-injection seam. It still uses the production
 * malloc/free owner and only suppresses the selected allocation attempt. */
toad_registration_status_t toad_env_register_funcs_checked_with_fault(
    toad_env_t *env, const toad_native_registration_t *registrations,
    size_t registration_count, toad_registration_fault_fn should_fail,
    void *fault_user_data);

#endif
