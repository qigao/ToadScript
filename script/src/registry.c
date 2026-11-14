/**
 * @file registry.c
 * @brief Registers TurboScript specific modules into the Engine global registry.
 */
#include <turboscript/engine.h>
#include <turboscript/module.h>

#include "internal.h"

// Forward declarations for engine built-in modules
extern const tbs_module_t *tbs_module_math(void);
extern const tbs_module_t *tbs_module_string(void);
extern const tbs_module_t *tbs_module_stats(void);
extern const tbs_module_t *tbs_module_io(void);
extern const tbs_module_t *tbs_module_core(void);
extern const tbs_module_t *tbs_module_regex(void);

void turbo_script_register_modules(void) {
  tbs_registry_add_module(tbs_module_math());
  tbs_registry_add_module(tbs_module_string());
  tbs_registry_add_module(tbs_module_stats());
  tbs_registry_add_module(tbs_module_io());
  tbs_registry_add_module(tbs_module_core());
  tbs_registry_add_module(tbs_module_regex());

  tbs_registry_init(); // Sort the registry
}
