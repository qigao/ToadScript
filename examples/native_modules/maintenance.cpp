#include <toad/plugin.h>

TOAD_PLUGIN_FUNCTION(double, plant_cost, (double, risk, CMETA_PARAM_IN),
                     (double, unit_cost, CMETA_PARAM_IN));
double plant_cost(double risk, double unit_cost) { return risk * risk * unit_cost; }

TOAD_PLUGIN_FUNCTION0(int, plant_limit);
int plant_limit() { return 42; }

#define MAINTENANCE_EXPORTS(X)                                                                     \
  X(function, plant_cost, "plant_cost", "example.maintenance", 1u, 0u)                             \
  X(function, plant_limit, "plant_limit", "example.maintenance", 1u, 0u)

CMETA_PLUGIN_DECLARE(maintenance, "example.maintenance", (1u, 0u, 0u), MAINTENANCE_EXPORTS,
                     CMETA_PLUGIN_PASSIVE());
