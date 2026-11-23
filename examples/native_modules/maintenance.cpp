#include <toad/plugin.h>

FunctionInvokeDeclAsAbiResult(
    value, double, &cmeta_type_double, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE, plant_cost,
    (double, risk, CMETA_PARAM_IN, &cmeta_type_double, CMETA_ABI_SCALAR),
    (double, unit_cost, CMETA_PARAM_IN, &cmeta_type_double, CMETA_ABI_SCALAR));
double plant_cost(double risk, double unit_cost) { return risk * risk * unit_cost; }

Function0InvokeDeclAsAbiResult(value, int, &cmeta_type_int, CMETA_ABI_SCALAR,
                               CMETA_RESULT_VALUE, plant_limit);
int plant_limit() { return 42; }

#define MAINTENANCE_EXPORTS(X)                                                                     \
  X(function, plant_cost, "plant_cost", "example.maintenance", 1u, 0u)                             \
  X(function, plant_limit, "plant_limit", "example.maintenance", 1u, 0u)

CMETA_PLUGIN_DECLARE(maintenance, "example.maintenance", (1u, 0u, 0u), MAINTENANCE_EXPORTS,
                     CMETA_PLUGIN_PASSIVE());
