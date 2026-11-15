#include <toad/plugin.h>

TOAD_PLUGIN_FUNCTION(bool, native_enabled, (bool, value, CMETA_PARAM_IN));
bool native_enabled(bool value) { return value; }
TOAD_PLUGIN_FUNCTION(long, native_counter, (long, value, CMETA_PARAM_IN));
long native_counter(long value) { return value; }
TOAD_PLUGIN_FUNCTION(float, native_sample, (float, value, CMETA_PARAM_IN));
float native_sample(float value) { return value; }
Function0InvokeDecl(value, void, native_notify);
static int notified;
void native_notify(void) { notified = 1; }
TOAD_PLUGIN_FUNCTION0(int, native_notified);
int native_notified(void) {
  int result = notified;
  notified = 0;
  return result;
}

#define SCALAR_EXPORTS(X)                                                                          \
  X(function, native_enabled, "native_enabled", "test.scalars", 1u, 0u)                            \
  X(function, native_counter, "native_counter", "test.scalars", 1u, 0u)                            \
  X(function, native_sample, "native_sample", "test.scalars", 1u, 0u)                              \
  X(function, native_notify, "native_notify", "test.scalars", 1u, 0u)                              \
  X(function, native_notified, "native_notified", "test.scalars", 1u, 0u)
CMETA_PLUGIN_DECLARE(scalars, "test.host.scalars", (1u, 0u, 0u), SCALAR_EXPORTS,
                     CMETA_PLUGIN_PASSIVE());
