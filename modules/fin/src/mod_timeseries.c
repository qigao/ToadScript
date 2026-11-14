/**
 * @file mod_timeseries.c
 * @brief Timeseries module: ts_diff/ts_autocorr/ts_pacf/ts_adf/ts_garch/ts_hurst
 */
#include <toad/module.h>
#include "ts_internal.h"
#include "ts.h"
#include <math.h>
#include <stdint.h>

static toad_value_t fn_ts_diff(size_t argc, toad_value_t *args, toad_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_diff(args[0].data.vector.data, n, (size_t)args[1].data.number, out, arena);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_autocorr(size_t argc, toad_value_t *args, toad_env_t *env,
                                     mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR) {
    size_t ml = (size_t)args[1].data.number;
    double *out = MEM_ALLOC_ARRAY(arena, double, ml + 1);
    if (out) {
      toad_ts_autocorr(args[0].data.vector.data, args[0].data.vector.size, ml, out);
      return toad_val_vec(out, ml + 1);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_pacf(size_t argc, toad_value_t *args, toad_env_t *env,
                                 mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR) {
    size_t ml = (size_t)args[1].data.number;
    double *out = MEM_ALLOC_ARRAY(arena, double, ml + 1);
    if (out) {
      toad_ts_pacf(args[0].data.vector.data, args[0].data.vector.size, ml, out, arena);
      return toad_val_vec(out, ml + 1);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_adf(size_t argc, toad_value_t *args, toad_env_t *env,
                                mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    size_t lag;
    if (args[1].type == TOAD_VAL_INTEGER && args[1].data.integer >= 0 &&
        (uint64_t)args[1].data.integer <= n) {
      lag = (size_t)args[1].data.integer;
    } else if (args[1].type == TOAD_VAL_NUMBER && isfinite(args[1].data.number) &&
               args[1].data.number >= 0.0 && args[1].data.number <= (double)n &&
               floor(args[1].data.number) == args[1].data.number) {
      lag = (size_t)args[1].data.number;
    } else {
      return toad_val_num(0);
    }
    double *out = MEM_ALLOC_ARRAY(arena, double, 2);
    if (out && toad_ts_adf(args[0].data.vector.data, n, lag, out, arena) == 2)
      return toad_val_vec(out, 2);
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_garch(size_t argc, toad_value_t *args, toad_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 3 && args[0].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_garch(args[0].data.vector.data, n, args[1].data.number, args[2].data.number, out);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_hurst(size_t argc, toad_value_t *args, toad_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 1 && args[0].type == TOAD_VAL_VECTOR)
    return toad_val_num(
        toad_ts_hurst(args[0].data.vector.data, args[0].data.vector.size, NULL, arena));
  return toad_val_num(0);
}

static toad_value_t fn_ts_match(size_t argc, toad_value_t *args, toad_env_t *env,
                                  mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_match(args[0].data.vector.data, args[1].data.vector.data, n, m, out);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_match_cosine(size_t argc, toad_value_t *args, toad_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  (void)arena;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_match_cosine(args[0].data.vector.data, args[1].data.vector.data, n, m, out);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_match_normalized(size_t argc, toad_value_t *args, toad_env_t *env,
                                             mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_match_normalized(args[0].data.vector.data, args[1].data.vector.data, n, m, out,
                                 arena);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_match_candle(size_t argc, toad_value_t *args, toad_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  if (argc == 8 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR &&
      args[2].type == TOAD_VAL_VECTOR && args[3].type == TOAD_VAL_VECTOR &&
      args[4].type == TOAD_VAL_VECTOR && args[5].type == TOAD_VAL_VECTOR &&
      args[6].type == TOAD_VAL_VECTOR && args[7].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size, m = args[4].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_match_candle(args[0].data.vector.data, args[1].data.vector.data,
                             args[2].data.vector.data, args[3].data.vector.data,
                             args[4].data.vector.data, args[5].data.vector.data,
                             args[6].data.vector.data, args[7].data.vector.data, n, m, out, arena);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_match_dtw(size_t argc, toad_value_t *args, toad_env_t *env,
                                      mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_match_dtw(args[0].data.vector.data, args[1].data.vector.data, n, m, out, arena);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_match_correl(size_t argc, toad_value_t *args, toad_env_t *env,
                                         mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_match_correl(args[0].data.vector.data, args[1].data.vector.data, n, m, out, arena);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_match_returns(size_t argc, toad_value_t *args, toad_env_t *env,
                                          mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size, m = args[1].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_match_returns(args[0].data.vector.data, args[1].data.vector.data, n, m, out, arena);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_coint(size_t argc, toad_value_t *args, toad_env_t *env,
                                   mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) {
    double *out = MEM_ALLOC_ARRAY(arena, double, 3);
    if (out) {
      size_t n = args[0].data.vector.size < args[1].data.vector.size
                     ? args[0].data.vector.size : args[1].data.vector.size;
      toad_ts_coint(args[0].data.vector.data, args[1].data.vector.data, n, out, arena);
      return toad_val_vec(out, 3);
    }
  }
  return toad_val_num(0);
}

static toad_value_t fn_ts_spread(size_t argc, toad_value_t *args, toad_env_t *env,
                                    mem_pool_t *arena) {
  (void)env;
  if (argc == 2 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) {
    size_t n = args[0].data.vector.size < args[1].data.vector.size
                   ? args[0].data.vector.size : args[1].data.vector.size;
    double *out = MEM_ALLOC_ARRAY(arena, double, n);
    if (out) {
      toad_ts_spread(args[0].data.vector.data, args[1].data.vector.data, n, out, arena);
      return toad_val_vec(out, n);
    }
  }
  return toad_val_num(0);
}

/* =========================================================================
 * Rolling / Expanding / EWM / Return wrappers
 * ========================================================================= */

#define ROLL_1V1P(name, func) \
static toad_value_t name(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena) { \
  (void)env; \
  if (argc >= 2 && args[0].type == TOAD_VAL_VECTOR) { \
    size_t n = args[0].data.vector.size, p = (size_t)args[1].data.number; \
    double *out = MEM_ALLOC_ARRAY(arena, double, n); \
    if (out && func(args[0].data.vector.data, n, p, out)) \
      return toad_val_vec(out, n); \
  } \
  return toad_val_num(0); \
}

ROLL_1V1P(fn_ts_rolling_mean, toad_ts_rolling_mean)
ROLL_1V1P(fn_ts_rolling_std,  toad_ts_rolling_std)
ROLL_1V1P(fn_ts_rolling_skew, toad_ts_rolling_skew)
ROLL_1V1P(fn_ts_rolling_kurt, toad_ts_rolling_kurt)
ROLL_1V1P(fn_ts_pct_change,   toad_ts_pct_change)
ROLL_1V1P(fn_ts_ewm_mean,     toad_ts_ewm_mean)
ROLL_1V1P(fn_ts_ewm_std,      toad_ts_ewm_std)

#define ROLL_2V1P(name, func) \
static toad_value_t name(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena) { \
  (void)env; \
  if (argc >= 3 && args[0].type == TOAD_VAL_VECTOR && args[1].type == TOAD_VAL_VECTOR) { \
    size_t n = args[0].data.vector.size < args[1].data.vector.size \
                   ? args[0].data.vector.size : args[1].data.vector.size; \
    size_t p = (size_t)args[2].data.number; \
    double *out = MEM_ALLOC_ARRAY(arena, double, n); \
    if (out && func(args[0].data.vector.data, args[1].data.vector.data, n, p, out)) \
      return toad_val_vec(out, n); \
  } \
  return toad_val_num(0); \
}

ROLL_2V1P(fn_ts_rolling_corr, toad_ts_rolling_corr)
ROLL_2V1P(fn_ts_rolling_beta, toad_ts_rolling_beta)

#define EXPAND_1V(name, func) \
static toad_value_t name(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena) { \
  (void)env; \
  if (argc >= 1 && args[0].type == TOAD_VAL_VECTOR) { \
    size_t n = args[0].data.vector.size; \
    double *out = MEM_ALLOC_ARRAY(arena, double, n); \
    if (out && func(args[0].data.vector.data, n, out)) \
      return toad_val_vec(out, n); \
  } \
  return toad_val_num(0); \
}

EXPAND_1V(fn_ts_expanding_mean, toad_ts_expanding_mean)
EXPAND_1V(fn_ts_expanding_std,  toad_ts_expanding_std)
EXPAND_1V(fn_ts_log_return,     toad_ts_log_return)
EXPAND_1V(fn_ts_cum_return,     toad_ts_cum_return)

static const toad_func_entry_t timeseries_entries[] = {
    {"adf", fn_ts_adf},
    {"autocorr", fn_ts_autocorr},
    {"coint", fn_ts_coint},
    {"cum_return", fn_ts_cum_return},
    {"diff", fn_ts_diff},
    {"ewm_mean", fn_ts_ewm_mean},
    {"ewm_std", fn_ts_ewm_std},
    {"expanding_mean", fn_ts_expanding_mean},
    {"expanding_std", fn_ts_expanding_std},
    {"garch", fn_ts_garch},
    {"hurst", fn_ts_hurst},
    {"log_return", fn_ts_log_return},
    {"match", fn_ts_match},
    {"match_candle", fn_ts_match_candle},
    {"match_correl", fn_ts_match_correl},
    {"match_cosine", fn_ts_match_cosine},
    {"match_dtw", fn_ts_match_dtw},
    {"match_normalized", fn_ts_match_normalized},
    {"match_returns", fn_ts_match_returns},
    {"pacf", fn_ts_pacf},
    {"pct_change", fn_ts_pct_change},
    {"rolling_beta", fn_ts_rolling_beta},
    {"rolling_corr", fn_ts_rolling_corr},
    {"rolling_kurt", fn_ts_rolling_kurt},
    {"rolling_mean", fn_ts_rolling_mean},
    {"rolling_skew", fn_ts_rolling_skew},
    {"rolling_std", fn_ts_rolling_std},
    {"spread", fn_ts_spread},
};

static const toad_module_t timeseries_module = {
    "ts", timeseries_entries, sizeof(timeseries_entries) / sizeof(timeseries_entries[0])};

const toad_module_t *toad_module_timeseries(void) { return &timeseries_module; }
