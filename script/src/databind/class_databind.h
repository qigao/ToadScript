#ifndef TOAD_SCRIPT_CLASS_DATABIND_H
#define TOAD_SCRIPT_CLASS_DATABIND_H

#include <toad/class.h>

#include <data_bind.h>
#include <data_bind_message_plan.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compile one typed Toad class into the canonical provider-backed
 * DataBind object plan used by mapper/db and other typed consumers.
 *
 * On success, caller owns both out_codec and out_plan. The plan borrows only
 * immutable class/CMeta metadata and therefore must not outlive the class.
 *
 * Returns:
 *   1  success
 *   0  contract/compile/allocation error (error is populated when possible)
 *  -1  class contains a field outside the currently supported typed DataBind
 *      domain; caller may choose a non-typed compatibility path.
 */
int toad_script_class_databind_compile(
    toad_class_t *klass,
    DataBind **out_codec,
    DataBindMessagePlan **out_plan,
    char *error,
    size_t error_len);

#ifdef __cplusplus
}
#endif

#endif /* TOAD_SCRIPT_CLASS_DATABIND_H */
