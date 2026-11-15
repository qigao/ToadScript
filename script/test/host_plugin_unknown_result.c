#include <toad/plugin.h>

TOAD_PLUGIN_FUNCTION0(int, native_admissible);
int native_admissible(void) { return 7; }

/* A callable adapter alone does not establish result ownership. */
FunctionInvokeDecl(value, double, native_unknown, (double, value, CMETA_PARAM_IN));
double native_unknown(double value) { return value; }

#define UNKNOWN_EXPORTS(X)                                                                         \
  X(function, native_admissible, "native_admissible", "test.host", 1u, 0u)                         \
  X(function, native_unknown, "native_unknown", "test.host", 1u, 0u)
CMETA_PLUGIN_DECLARE(unknown, "test.host.unknown", (1u, 0u, 0u), UNKNOWN_EXPORTS,
                     CMETA_PLUGIN_PASSIVE());
