/**
 * @file registry.c
 * @brief Registers Toad specific modules into the Engine global registry.
 */
#include <toad/engine.h>
#include <toad/module.h>

#include "internal.h"

// Forward declarations for engine built-in modules
extern const toad_module_t *toad_module_math(void);
extern const toad_module_t *toad_module_string(void);
extern const toad_module_t *toad_module_stats(void);
extern const toad_module_t *toad_module_io(void);
extern const toad_module_t *toad_module_core(void);
extern const toad_module_t *toad_module_regex(void);

void toad_script_register_modules(void) {
  toad_registry_add_module(toad_module_math());
  toad_registry_add_module(toad_module_string());
  toad_registry_add_module(toad_module_stats());
  toad_registry_add_module(toad_module_io());
  toad_registry_add_module(toad_module_core());
  toad_registry_add_module(toad_module_regex());

  toad_registry_init(); // Sort the registry
}
