/**
 * @file fin.h
 * @brief Finance module public API.
 *
 * Declares Finance-domain C APIs used by the fin module
 * (backtest, risk, portfolio, signals, factors, market microstructure).
 */
#ifndef FIN_H
#define FIN_H

#include <toad/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Backtest
 *
 * toad_bt_backtest() is a same-bar-open toy simulator:
 * signal[i] is executed at open[i], then equity[i] is marked with close[i].
 * Callers that derive signal from close[i] must lag the signal themselves
 * to avoid look-ahead bias.
 *
 * trades[] is a sparse per-bar P&L array with length n, not a compact trade log.
 *
 * toad_bt_cost() is a side-agnostic helper:
 * tax_pct is applied to notional on both buys and sells. Market-specific
 * one-way taxes such as CN stamp duty belong in market_rules/market_commission().
 */
/* ========================================================================= */
size_t toad_bt_backtest(const double *open, const double *close, const double *signal, size_t n, double cash0, double commission, double *equity, double *trades);
size_t toad_bt_stats(const double *equity, const double *trades, size_t n, size_t num_trades, double annual, double *out);
size_t toad_bt_backtest_ex(const double *open, const double *high, const double *low, const double *close, const double *volume, const double *signal, size_t n, double cash0, double commission, const double *config, size_t config_n, double *equity, double *trades, mem_pool_t *arena);
size_t toad_bt_portfolio(const double *prices, const double *signals, size_t na, size_t nb, double cash0, double commission, double *equity, double *weights, mem_pool_t *arena);
double toad_bt_slippage(double price, double size, double vol, double avg_vol, double lambda);
double toad_bt_cost(double price, double size, double commission_pct, double tax_pct, double slippage);

/* Portfolio */
/* toad_pf_cov_matrix() returns the population covariance matrix (divide by np),
 * not the sample covariance (np - 1).
 */
void   toad_pf_cov_matrix(const double *returns, size_t na, size_t np, double *out, mem_pool_t *arena);
double toad_pf_min_variance(const double *cov, size_t n, double *weights, mem_pool_t *arena);
/* Unconstrained tangency portfolio: weights sum to 1, but may be negative or >1. */
double toad_pf_max_sharpe(const double *mu, const double *cov, size_t n, double rf, double *weights, mem_pool_t *arena);
double toad_pf_markowitz(const double *mu, const double *cov, size_t n, double target, double *weights, mem_pool_t *arena);
double toad_pf_risk_parity(const double *cov, size_t n, double *weights, mem_pool_t *arena);

/* Risk */
double toad_fixed_frac(double equity, double risk_pct, double stop_dist);
double toad_optimal_f(const double *trades, size_t n, double *out);
/* `mu` and `sigma` are annualized, `dt` is the time-step expressed as a year fraction. */
size_t toad_mc_simulate(double s0, double mu, double sigma, double dt, size_t steps, size_t paths, double *out, mem_pool_t *arena);

/* Performance Metrics */
double toad_sharpe(const double *returns, size_t n, double rf, double annual_factor);
double toad_sortino(const double *returns, size_t n, double rf, double annual_factor);
double toad_calmar(const double *equity, size_t n, double annual_factor);
double toad_profit_factor(const double *trades, size_t n);
double toad_expectancy(const double *trades, size_t n);
double toad_payoff_ratio(const double *trades, size_t n);
size_t toad_max_dd_duration(const double *equity, size_t n);
double toad_ulcer_index(const double *equity, size_t n);
double toad_information_ratio(const double *ret, const double *bench, size_t n);
double toad_treynor(const double *ret, const double *bench, size_t n, double rf);

/* Signal */
/* Note: crossover, crossunder, candle_doji, candle_hammer are in ta module */
size_t toad_signal_combine(const double *signals, const double *weights, size_t n_bars, size_t n_signals, double *out);
size_t toad_candle_engulfing(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);
size_t toad_candle_morningstar(const double *O, const double *H, const double *L, const double *C, size_t n, double *out);


/* ── Factor Processing ── */
size_t toad_vec_rank(const double *in, size_t n, double *out, mem_pool_t *arena);
size_t toad_vec_zscore(const double *in, size_t n, double *out, mem_pool_t *arena);
size_t toad_vec_winsorize(const double *in, size_t n, double limit_pct, double *out, mem_pool_t *arena);
size_t toad_vec_standardize(const double *in, size_t n, double *out, mem_pool_t *arena);
size_t toad_ta_zscore(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
double toad_vec_entropy(const double *in, size_t n, size_t bins);
double toad_vec_skewness(const double *in, size_t n);
double toad_vec_kurtosis(const double *in, size_t n);
double toad_vec_vol_ratio(const double *vol, size_t n, size_t segment_len, size_t segment_idx);
double toad_vec_rvar(const double *in, size_t n);
double toad_vec_rskew(const double *in, size_t n);
double toad_vec_rkurt(const double *in, size_t n);
double toad_vec_illiq(const double *ret, const double *amount, size_t n);
double toad_vec_trend_strength(const double *price, size_t n);
size_t toad_vec_csad(const double *rets, size_t na, size_t np, double *out, mem_pool_t *arena);
double toad_vec_fvd(const double *vol, size_t n, size_t window_size);
double toad_vec_efficiency(const double *price, size_t n);
double toad_vec_rsj(const double *ret, size_t n);
double toad_vec_apm(const double *ret_am, const double *ret_pm, size_t n);
size_t toad_vec_cgo(const double *p, const double *v, size_t n, size_t period, double *out, mem_pool_t *arena);
size_t toad_vec_quantile(const double *in, size_t n, size_t period, double *out, mem_pool_t *arena);
double toad_vec_salience(const double *ret, const double *mkt_ret, size_t n, double delta);
double toad_vec_str(const double *ret, const double *mkt_ret, size_t n, double delta);

/* ── Tick / Order Book Analysis ── */
double toad_tick_ofi(double b_p, double b_v, double a_p, double a_v,
                       double pb_p, double pb_v, double pa_p, double pa_v);
double toad_tick_imbalance(const double *bids_v, const double *asks_v, size_t depth);
double toad_tick_limit_status(double price, double prev_close, double limit_pct);

/* ── Graph Algorithms (Network Analysis) ── */
toad_value_t toad_graph_bellman_ford(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_graph_has_negative_cycle(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_graph_extract_path(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_graph_detect_arbitrage(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_game(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_support_enumeration(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_vertex_enumeration(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_lemke_howson(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_fictitious_play(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_stochastic_fictitious_play(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_discrete_replicator_dynamics(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_imitation_dynamics(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_regret_minimization(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_introspection_dynamics(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_repeated_game(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_moran_process(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_replicator_mutation(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_asymmetric_replicator(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_asymmetric_replicator_derivative(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);
toad_value_t toad_nash_replicator(size_t argc, toad_value_t *args, toad_env_t *env, mem_pool_t *arena);

/* ── Event-driven strategy module (buy/sell/flat, rank, risk sizing, ...) ── */
const toad_module_t *toad_module_strategy(void);

#ifdef __cplusplus
}
#endif

#endif /* FIN_H */
