#ifndef TBS_RUNTIME_INTERNAL_H
#define TBS_RUNTIME_INTERNAL_H

#include <turboscript/engine.h>

typedef enum tbs_registration_status_e {
    TBS_REGISTRATION_OK = 0,
    TBS_REGISTRATION_INVALID_ARGUMENT = 1,
    TBS_REGISTRATION_CONFLICT = 2,
    TBS_REGISTRATION_OUT_OF_MEMORY = 3,
} tbs_registration_status_t;

typedef struct tbs_native_registration_s {
    const char *name;
    tbs_native_fn fn;
    void *user_data;
    tbs_native_flags_t flags;
} tbs_native_registration_t;

typedef int (*tbs_registration_fault_fn)(size_t allocation_index,
                                            void *user_data);

tbs_registration_status_t tbs_env_register_funcs_checked(
    tbs_env_t *env, const tbs_native_registration_t *registrations,
    size_t registration_count);

/* Internal deterministic fault-injection seam. It still uses the production
 * malloc/free owner and only suppresses the selected allocation attempt. */
tbs_registration_status_t tbs_env_register_funcs_checked_with_fault(
    tbs_env_t *env, const tbs_native_registration_t *registrations,
    size_t registration_count, tbs_registration_fault_fn should_fail,
    void *fault_user_data);

#endif
