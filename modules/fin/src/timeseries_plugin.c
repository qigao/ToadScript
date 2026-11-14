#include <toad/plugin.h>

const toad_module_t *toad_module_timeseries(void);

TOAD_PLUGIN_MODULE(ts, toad_module_timeseries)
