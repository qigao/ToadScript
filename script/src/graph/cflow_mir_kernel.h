#ifndef TOAD_SCRIPT_CFLOW_MIR_KERNEL_H
#define TOAD_SCRIPT_CFLOW_MIR_KERNEL_H

#include "cmeta_bridge.h"
#include <toad/script.h>
#include <cmeta/cmeta.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ts_cflow_mir_kernel_s ts_cflow_mir_kernel_t;

/*
 * Toad owns the MIR kernel. CFlow may copy callable by value into Graph
 * and Plan snapshots, so owner must outlive every Graph/Plan that contains
 * callable. Destroy Plan/Graph first, then destroy this binding.
 */
typedef struct ts_cflow_mir_kernel_binding {
  cmeta_callable callable;
  ts_cflow_mir_kernel_t *owner;
} ts_cflow_mir_kernel_binding_t;

/*
 * Compile one PURE, capture-free Toad lambda into a MIR-backed CMeta
 * callable. MAP/FILTER use one numeric argument; REDUCE uses the canonical
 * homogeneous (accumulator, value) -> accumulator contract.
 */
bool ts_cflow_mir_kernel_bind(toad_script_ctx_t *runtime_ctx,
                              const toad_node_t *lambda,
                              ts_cmeta_lambda_role_t role,
                              ts_cflow_mir_kernel_binding_t *out,
                              const char **error);

void ts_cflow_mir_kernel_binding_destroy(
    ts_cflow_mir_kernel_binding_t *binding);

#ifdef __cplusplus
}
#endif

#endif /* TOAD_SCRIPT_CFLOW_MIR_KERNEL_H */
