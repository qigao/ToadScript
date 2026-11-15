#include <toad/plugin.h>

TOAD_PLUGIN_FUNCTION(double, plant_normalize, (double, value, CMETA_PARAM_IN),
                     (double, baseline, CMETA_PARAM_IN));
double plant_normalize(double value, double baseline) { return (value - baseline) / baseline; }

TOAD_PLUGIN_FUNCTION(double, plant_gain, (double, value, CMETA_PARAM_IN));
double plant_gain(double value) { return value * 2.0; }

TOAD_PLUGIN_FUNCTION(bool, plant_positive, (int, value, CMETA_PARAM_IN));
bool plant_positive(int value) { return value > 0; }

#define CALIBRATION_EXPORTS(X)                                                                     \
  X(function, plant_normalize, "plant_normalize", "example.calibration", 1u, 0u)                   \
  X(function, plant_gain, "plant_gain", "example.calibration", 1u, 0u)                             \
  X(function, plant_positive, "plant_positive", "example.calibration", 1u, 0u)

CMETA_PLUGIN_DECLARE(calibration, "example.calibration", (1u, 0u, 0u), CALIBRATION_EXPORTS,
                     CMETA_PLUGIN_PASSIVE());
