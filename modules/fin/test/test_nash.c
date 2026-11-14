/**
 * @file test_nash.c
 * @brief Tests for the Toad Nash helpers in the finance module.
 */

#include "fin.h"
#include <toad/engine.h>
#include <toad/module.h>
#include "tinytest.h"
#include <math.h>
#include <string.h>

#define EPSILON 0.05

suite("Nash Equilibrium") {
  group("game metadata") {
    it("classifies a zero-sum matching-pennies game") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[4];
      toad_value_t result;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 4096);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);

      result = toad_nash_game(4, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      check(fabs((double)(toad_map_get(&result, "rows").data.number) - (double)(2.0)) <= (double)(1e-9));
      check(fabs((double)(toad_map_get(&result, "cols").data.number) - (double)(2.0)) <= (double)(1e-9));
      check(fabs((double)(toad_map_get(&result, "zero_sum").data.number) - (double)(1.0)) <= (double)(1e-9));
      check(fabs((double)(toad_map_get(&result, "symmetric").data.number) - (double)(0.0)) <= (double)(1e-9));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("support enumeration") {
    it("matches the Nashpy README bimatrix example with three equilibria") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[5];
      toad_value_t result;
      toad_value_t equilibria;
      double row_payoffs[] = {1.0, 2.0, 3.0, 0.0};
      double col_payoffs[] = {0.0, 2.0, 3.0, 1.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(1e-8);

      result = toad_nash_support_enumeration(5, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      check(fabs((double)(toad_map_get(&result, "count").data.number) - (double)(3.0)) <= (double)(1e-9));

      equilibria = toad_map_get(&result, "equilibria");
      check((equilibria.type) == (TOAD_VAL_LIST));
      check(((int)equilibria.data.list.count) == (3));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("matches Nashpy's 2x2 support-enumeration fixture") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[5];
      toad_value_t result;
      toad_value_t equilibria;
      int found_pure_00 = 0;
      int found_pure_11 = 0;
      int found_mixed = 0;
      double row_payoffs[] = {2.0, 1.0, 0.0, 2.0};
      double col_payoffs[] = {2.0, 0.0, 1.0, 2.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(1e-8);

      result = toad_nash_support_enumeration(5, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      check(fabs((double)(toad_map_get(&result, "count").data.number) - (double)(3.0)) <= (double)(1e-9));

      equilibria = toad_map_get(&result, "equilibria");
      check((equilibria.type) == (TOAD_VAL_LIST));
      for (size_t i = 0; i < equilibria.data.list.count; i++) {
        toad_value_t eq = equilibria.data.list.items[i];
        toad_value_t row_vec = toad_map_get(&eq, "row_strategy");
        toad_value_t col_vec = toad_map_get(&eq, "col_strategy");

        if (fabs(row_vec.data.vector.data[0] - 1.0) < 1e-9 &&
            fabs(col_vec.data.vector.data[0] - 1.0) < 1e-9) {
          found_pure_00 = 1;
        }
        if (fabs(row_vec.data.vector.data[1] - 1.0) < 1e-9 &&
            fabs(col_vec.data.vector.data[1] - 1.0) < 1e-9) {
          found_pure_11 = 1;
        }
        if (fabs(row_vec.data.vector.data[0] - (1.0 / 3.0)) < 1e-6 &&
            fabs(row_vec.data.vector.data[1] - (2.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[0] - (1.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[1] - (2.0 / 3.0)) < 1e-6) {
          found_mixed = 1;
        }
      }

      check(found_pure_00);
      check(found_pure_11);
      check(found_mixed);

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("finds the mixed equilibrium of matching pennies") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[5];
      toad_value_t result;
      toad_value_t equilibria;
      toad_value_t eq0;
      toad_value_t row_vec;
      toad_value_t col_vec;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(1e-8);

      result = toad_nash_support_enumeration(5, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      check(fabs((double)(toad_map_get(&result, "count").data.number) - (double)(1.0)) <= (double)(1e-9));

      equilibria = toad_map_get(&result, "equilibria");
      check((equilibria.type) == (TOAD_VAL_LIST));
      check(((int)equilibria.data.list.count) == (1));

      eq0 = equilibria.data.list.items[0];
      row_vec = toad_map_get(&eq0, "row_strategy");
      col_vec = toad_map_get(&eq0, "col_strategy");

      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(0.5)) <= (double)(EPSILON));
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(0.5)) <= (double)(EPSILON));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(0.5)) <= (double)(EPSILON));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(0.5)) <= (double)(EPSILON));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("vertex enumeration") {
    it("matches Nashpy's 3x2 vertex-enumeration fixture") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[5];
      toad_value_t result;
      toad_value_t equilibria;
      int found_pure = 0;
      int found_mixed_a = 0;
      int found_mixed_b = 0;
      double row_payoffs[] = {
        3.0, 3.0,
        2.0, 5.0,
        0.0, 6.0
      };
      double col_payoffs[] = {
        3.0, 2.0,
        2.0, 6.0,
        3.0, 1.0
      };

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 6);
      args[1] = toad_val_vec(col_payoffs, 6);
      args[2] = toad_val_num(3.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(1e-8);

      result = toad_nash_vertex_enumeration(5, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      check(fabs((double)(toad_map_get(&result, "count").data.number) - (double)(3.0)) <= (double)(1e-9));

      equilibria = toad_map_get(&result, "equilibria");
      for (size_t i = 0; i < equilibria.data.list.count; i++) {
        toad_value_t eq = equilibria.data.list.items[i];
        toad_value_t row_vec = toad_map_get(&eq, "row_strategy");
        toad_value_t col_vec = toad_map_get(&eq, "col_strategy");

        if (fabs(row_vec.data.vector.data[0] - 1.0) < 1e-9 &&
            fabs(col_vec.data.vector.data[0] - 1.0) < 1e-9) {
          found_pure = 1;
        }
        if (fabs(row_vec.data.vector.data[1] - (1.0 / 3.0)) < 1e-6 &&
            fabs(row_vec.data.vector.data[2] - (2.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[0] - (1.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[1] - (2.0 / 3.0)) < 1e-6) {
          found_mixed_a = 1;
        }
        if (fabs(row_vec.data.vector.data[0] - 0.8) < 1e-6 &&
            fabs(row_vec.data.vector.data[1] - 0.2) < 1e-6 &&
            fabs(col_vec.data.vector.data[0] - (2.0 / 3.0)) < 1e-6 &&
            fabs(col_vec.data.vector.data[1] - (1.0 / 3.0)) < 1e-6) {
          found_mixed_b = 1;
        }
      }

      check(found_pure);
      check(found_mixed_a);
      check(found_mixed_b);

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("lemke howson") {
    it("matches the Nashpy 3x2 fixture for selected dropped labels") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[5];
      toad_value_t eq;
      toad_value_t row_vec;
      toad_value_t col_vec;
      double row_payoffs[] = {
        3.0, 3.0,
        2.0, 5.0,
        0.0, 6.0
      };
      double col_payoffs[] = {
        3.0, 2.0,
        2.0, 6.0,
        3.0, 1.0
      };

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 6);
      args[1] = toad_val_vec(col_payoffs, 6);
      args[2] = toad_val_num(3.0);
      args[3] = toad_val_num(2.0);

      args[4] = toad_val_num(0.0);
      eq = toad_nash_lemke_howson(5, args, &env, &arena);
      check((toad_map_get(&eq, "engine").type) == (TOAD_VAL_STRING));
      check(strncmp(toad_map_get(&eq, "engine").data.string.data, "tableau", 7) == 0);
      row_vec = toad_map_get(&eq, "row_strategy");
      col_vec = toad_map_get(&eq, "col_strategy");
      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(1.0)) <= (double)(1e-9));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(1.0)) <= (double)(1e-9));
      toad_map_free(&eq);

      args[4] = toad_val_num(1.0);
      eq = toad_nash_lemke_howson(5, args, &env, &arena);
      row_vec = toad_map_get(&eq, "row_strategy");
      col_vec = toad_map_get(&eq, "col_strategy");
      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(0.0)) <= (double)(1e-6));
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(1.0 / 3.0)) <= (double)(1e-6));
      check(fabs((double)(row_vec.data.vector.data[2]) - (double)(2.0 / 3.0)) <= (double)(1e-6));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(1.0 / 3.0)) <= (double)(1e-6));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(2.0 / 3.0)) <= (double)(1e-6));
      toad_map_free(&eq);

      args[4] = toad_val_num(4.0);
      eq = toad_nash_lemke_howson(5, args, &env, &arena);
      row_vec = toad_map_get(&eq, "row_strategy");
      col_vec = toad_map_get(&eq, "col_strategy");
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(1.0 / 3.0)) <= (double)(1e-6));
      check(fabs((double)(row_vec.data.vector.data[2]) - (double)(2.0 / 3.0)) <= (double)(1e-6));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(1.0 / 3.0)) <= (double)(1e-6));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(2.0 / 3.0)) <= (double)(1e-6));
      toad_map_free(&eq);

      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("fictitious play") {
    it("tracks Nashpy's 3x3 fictitious-play fixture toward the symmetric mixture") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[5];
      toad_value_t result;
      toad_value_t row_vec;
      toad_value_t col_vec;
      double row_payoffs[] = {
        0.5, 1.0, 0.0,
        0.0, 0.5, 1.0,
        1.0, 0.0, 0.5
      };
      double col_payoffs[] = {
        0.5, 0.0, 1.0,
        1.0, 0.5, 0.0,
        0.0, 1.0, 0.5
      };

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 9);
      args[1] = toad_val_vec(col_payoffs, 9);
      args[2] = toad_val_num(3.0);
      args[3] = toad_val_num(3.0);
      args[4] = toad_val_num(10000.0);

      result = toad_nash_fictitious_play(5, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      row_vec = toad_map_get(&result, "row_strategy");
      col_vec = toad_map_get(&result, "col_strategy");

      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(1.0 / 3.0)) <= (double)(0.08));
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(1.0 / 3.0)) <= (double)(0.08));
      check(fabs((double)(row_vec.data.vector.data[2]) - (double)(1.0 / 3.0)) <= (double)(0.08));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(1.0 / 3.0)) <= (double)(0.08));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(1.0 / 3.0)) <= (double)(0.08));
      check(fabs((double)(col_vec.data.vector.data[2]) - (double)(1.0 / 3.0)) <= (double)(0.08));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("tracks empirical play toward the matching-pennies mixture") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[5];
      toad_value_t result;
      toad_value_t row_vec;
      toad_value_t col_vec;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(2000.0);

      result = toad_nash_fictitious_play(5, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      row_vec = toad_map_get(&result, "row_strategy");
      col_vec = toad_map_get(&result, "col_strategy");

      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(0.5)) <= (double)(0.15));
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(0.5)) <= (double)(0.15));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(0.5)) <= (double)(0.15));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(0.5)) <= (double)(0.15));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("stochastic fictitious play is deterministic for a fixed seed and stays near the mixture") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[7];
      toad_value_t first;
      toad_value_t second;
      toad_value_t row_first;
      toad_value_t col_first;
      toad_value_t row_second;
      toad_value_t col_second;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(4000.0);
      args[5] = toad_val_num(0.2);
      args[6] = toad_val_num(7.0);

      first = toad_nash_stochastic_fictitious_play(7, args, &env, &arena);
      second = toad_nash_stochastic_fictitious_play(7, args, &env, &arena);
      check((first.type) == (TOAD_VAL_MAP));
      check((second.type) == (TOAD_VAL_MAP));

      row_first = toad_map_get(&first, "row_strategy");
      col_first = toad_map_get(&first, "col_strategy");
      row_second = toad_map_get(&second, "row_strategy");
      col_second = toad_map_get(&second, "col_strategy");

      check(fabs((double)(row_first.data.vector.data[0]) - (double)(row_second.data.vector.data[0])) <= (double)(1e-12));
      check(fabs((double)(row_first.data.vector.data[1]) - (double)(row_second.data.vector.data[1])) <= (double)(1e-12));
      check(fabs((double)(col_first.data.vector.data[0]) - (double)(col_second.data.vector.data[0])) <= (double)(1e-12));
      check(fabs((double)(col_first.data.vector.data[1]) - (double)(col_second.data.vector.data[1])) <= (double)(1e-12));

      check(fabs((double)(row_first.data.vector.data[0]) - (double)(0.5)) <= (double)(0.12));
      check(fabs((double)(row_first.data.vector.data[1]) - (double)(0.5)) <= (double)(0.12));
      check(fabs((double)(col_first.data.vector.data[0]) - (double)(0.5)) <= (double)(0.12));
      check(fabs((double)(col_first.data.vector.data[1]) - (double)(0.5)) <= (double)(0.12));

      toad_map_free(&first);
      toad_map_free(&second);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("discrete replicator dynamics") {
    it("matches Nashpy's one-generation quantized update") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[6];
      toad_value_t result;
      toad_value_t history;
      double payoffs[] = {
        4.0, 3.0,
        2.0, 5.0
      };
      double population[] = {20.0, 80.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(payoffs, 4);
      args[1] = toad_val_num(2.0);
      args[2] = toad_val_vec(population, 2);
      args[3] = toad_val_num(1.0);
      args[4] = toad_val_num(1.0);
      args[5] = toad_val_num(2.0);

      result = toad_nash_discrete_replicator_dynamics(6, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      history = toad_map_get(&result, "history");
      check((history.type) == (TOAD_VAL_LIST));
      check(((int)history.data.list.count) == (1));
      check(fabs((double)(history.data.list.items[0].data.vector.data[0]) - (double)(15.0)) <= (double)(1e-9));
      check(fabs((double)(history.data.list.items[0].data.vector.data[1]) - (double)(85.0)) <= (double)(1e-9));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("matches Nashpy's type-1 single-step counts") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[6];
      toad_value_t result;
      toad_value_t population_out;
      double payoffs[] = {
        4.0, 3.0,
        2.0, 5.0
      };
      double population[] = {20.0, 80.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(payoffs, 4);
      args[1] = toad_val_num(2.0);
      args[2] = toad_val_vec(population, 2);
      args[3] = toad_val_num(1.0);
      args[4] = toad_val_num(0.0);
      args[5] = toad_val_num(1.0);

      result = toad_nash_discrete_replicator_dynamics(6, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      population_out = toad_map_get(&result, "population");
      check(fabs((double)(population_out.data.vector.data[0]) - (double)(0.8)) <= (double)(1e-9));
      check(fabs((double)(population_out.data.vector.data[1]) - (double)(99.2)) <= (double)(1e-9));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("imitation dynamics") {
    it("keeps thresholded strategies in range for positive and negative payoffs") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[8];
      toad_value_t result;
      toad_value_t row_vec;
      toad_value_t col_vec;
      toad_value_t raw_row;
      toad_value_t raw_col;
      double row_positive[] = {3.0, 0.0, 1.0, 3.0};
      double col_positive[] = {0.0, 1.0, 3.0, 0.0};
      double row_negative[] = {-1.0, 0.0, 0.0, -1.0};
      double col_negative[] = {0.0, -1.0, -1.0, 0.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_positive, 4);
      args[1] = toad_val_vec(col_positive, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(32.0);
      args[5] = toad_val_num(64.0);
      args[6] = toad_val_num(17.0);
      args[7] = toad_val_num(0.5);

      result = toad_nash_imitation_dynamics(8, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      row_vec = toad_map_get(&result, "row_strategy");
      col_vec = toad_map_get(&result, "col_strategy");
      raw_row = toad_map_get(&result, "raw_row_strategy");
      raw_col = toad_map_get(&result, "raw_col_strategy");
      for (size_t i = 0; i < row_vec.data.vector.size; i++) {
        check(row_vec.data.vector.data[i] >= 0.0 && row_vec.data.vector.data[i] <= 1.0);
        check(raw_row.data.vector.data[i] >= 0.0 && raw_row.data.vector.data[i] <= 1.0);
      }
      for (size_t i = 0; i < col_vec.data.vector.size; i++) {
        check(col_vec.data.vector.data[i] >= 0.0 && col_vec.data.vector.data[i] <= 1.0);
        check(raw_col.data.vector.data[i] >= 0.0 && raw_col.data.vector.data[i] <= 1.0);
      }
      toad_map_free(&result);

      args[0] = toad_val_vec(row_negative, 4);
      args[1] = toad_val_vec(col_negative, 4);
      result = toad_nash_imitation_dynamics(8, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      row_vec = toad_map_get(&result, "row_strategy");
      col_vec = toad_map_get(&result, "col_strategy");
      for (size_t i = 0; i < row_vec.data.vector.size; i++) {
        check(row_vec.data.vector.data[i] >= 0.0 && row_vec.data.vector.data[i] <= 1.0);
      }
      for (size_t i = 0; i < col_vec.data.vector.size; i++) {
        check(col_vec.data.vector.data[i] >= 0.0 && col_vec.data.vector.data[i] <= 1.0);
      }

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("is deterministic for a fixed seed") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[8];
      toad_value_t first;
      toad_value_t second;
      toad_value_t row_first;
      toad_value_t col_first;
      toad_value_t row_second;
      toad_value_t col_second;
      double row_payoffs[] = {3.0, 0.0, 1.0, 3.0};
      double col_payoffs[] = {0.0, 1.0, 3.0, 0.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(50.0);
      args[5] = toad_val_num(50.0);
      args[6] = toad_val_num(23.0);
      args[7] = toad_val_num(0.5);

      first = toad_nash_imitation_dynamics(8, args, &env, &arena);
      second = toad_nash_imitation_dynamics(8, args, &env, &arena);
      check((first.type) == (TOAD_VAL_MAP));
      check((second.type) == (TOAD_VAL_MAP));

      row_first = toad_map_get(&first, "row_strategy");
      col_first = toad_map_get(&first, "col_strategy");
      row_second = toad_map_get(&second, "row_strategy");
      col_second = toad_map_get(&second, "col_strategy");
      check(fabs((double)(row_first.data.vector.data[0]) - (double)(row_second.data.vector.data[0])) <= (double)(1e-12));
      check(fabs((double)(row_first.data.vector.data[1]) - (double)(row_second.data.vector.data[1])) <= (double)(1e-12));
      check(fabs((double)(col_first.data.vector.data[0]) - (double)(col_second.data.vector.data[0])) <= (double)(1e-12));
      check(fabs((double)(col_first.data.vector.data[1]) - (double)(col_second.data.vector.data[1])) <= (double)(1e-12));

      toad_map_free(&first);
      toad_map_free(&second);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("regret minimization") {
    it("matches the Nashpy zero-sum rock-paper-scissors fixture") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[6];
      toad_value_t result;
      toad_value_t row_vec;
      toad_value_t col_vec;
      double row_payoffs[] = {
        0.0, -1.0, 1.0,
        1.0, 0.0, -1.0,
        -1.0, 1.0, 0.0
      };
      double col_payoffs[] = {
        0.0, 1.0, -1.0,
        -1.0, 0.0, 1.0,
        1.0, -1.0, 0.0
      };

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 9);
      args[1] = toad_val_vec(col_payoffs, 9);
      args[2] = toad_val_num(3.0);
      args[3] = toad_val_num(3.0);
      args[4] = toad_val_num(0.1);
      args[5] = toad_val_num(100.0);

      result = toad_nash_regret_minimization(6, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      row_vec = toad_map_get(&result, "row_strategy");
      col_vec = toad_map_get(&result, "col_strategy");
      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(1.0 / 3.0)) <= (double)(1e-12));
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(1.0 / 3.0)) <= (double)(1e-12));
      check(fabs((double)(row_vec.data.vector.data[2]) - (double)(1.0 / 3.0)) <= (double)(1e-12));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(1.0 / 3.0)) <= (double)(1e-12));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(1.0 / 3.0)) <= (double)(1e-12));
      check(fabs((double)(col_vec.data.vector.data[2]) - (double)(1.0 / 3.0)) <= (double)(1e-12));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("matches the Nashpy non-zero-sum pure-strategy fixture") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[6];
      toad_value_t result;
      toad_value_t row_vec;
      toad_value_t col_vec;
      double row_payoffs[] = {
        3.0, -1.0, 3.0,
        -1.0, 3.0, 6.0,
        -1.0, 1.0, 2.0
      };
      double col_payoffs[] = {
        -3.0, 1.0, 4.0,
        1.0, -3.0, 3.0,
        -1.0, 3.0, 4.0
      };

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 9);
      args[1] = toad_val_vec(col_payoffs, 9);
      args[2] = toad_val_num(3.0);
      args[3] = toad_val_num(3.0);
      args[4] = toad_val_num(0.1);
      args[5] = toad_val_num(100.0);

      result = toad_nash_regret_minimization(6, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      row_vec = toad_map_get(&result, "row_strategy");
      col_vec = toad_map_get(&result, "col_strategy");
      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(0.0)) <= (double)(1e-12));
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(1.0)) <= (double)(1e-12));
      check(fabs((double)(row_vec.data.vector.data[2]) - (double)(0.0)) <= (double)(1e-12));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(0.0)) <= (double)(1e-12));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(0.0)) <= (double)(1e-12));
      check(fabs((double)(col_vec.data.vector.data[2]) - (double)(1.0)) <= (double)(1e-12));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("introspection dynamics") {
    it("is deterministic for a fixed seed and preserves valid actions") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[8];
      toad_value_t first;
      toad_value_t second;
      toad_value_t history;
      double row_payoffs[] = {
        3.0, 4.0,
        5.0, 1.0,
        6.0, 3.0
      };
      double col_payoffs[] = {
        5.0, 2.0,
        3.0, 4.0,
        1.0, 9.0
      };
      double initial_actions[] = {2.0, 1.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 6);
      args[1] = toad_val_vec(col_payoffs, 6);
      args[2] = toad_val_num(3.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(10.0);
      args[5] = toad_val_num(1.0);
      args[6] = toad_val_num(11.0);
      args[7] = toad_val_vec(initial_actions, 2);

      first = toad_nash_introspection_dynamics(8, args, &env, &arena);
      second = toad_nash_introspection_dynamics(8, args, &env, &arena);
      check((first.type) == (TOAD_VAL_MAP));
      check((second.type) == (TOAD_VAL_MAP));
      check(fabs((double)(toad_map_get(&first, "steps").data.number) - (double)(11.0)) <= (double)(1e-9));
      check(fabs((double)(toad_map_get(&second, "steps").data.number) - (double)(11.0)) <= (double)(1e-9));

      history = toad_map_get(&first, "history");
      check((history.type) == (TOAD_VAL_LIST));
      check(((int)history.data.list.count) == (11));
      for (size_t i = 0; i < history.data.list.count; i++) {
        toad_value_t step_a = history.data.list.items[i];
        toad_value_t step_b = toad_map_get(&second, "history").data.list.items[i];
        check((step_a.type) == (TOAD_VAL_LIST));
        check(fabs((double)(step_a.data.list.items[0].data.number) - (double)(step_b.data.list.items[0].data.number)) <= (double)(1e-12));
        check(fabs((double)(step_a.data.list.items[1].data.number) - (double)(step_b.data.list.items[1].data.number)) <= (double)(1e-12));
        check(step_a.data.list.items[0].data.number >= 0.0 && step_a.data.list.items[0].data.number < 3.0);
        check(step_a.data.list.items[1].data.number >= 0.0 && step_a.data.list.items[1].data.number < 2.0);
      }

      toad_map_free(&first);
      toad_map_free(&second);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("repeated games") {
    it("matches Nashpy repeated-game dimensions for a 2x2 stage game repeated twice") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[5];
      toad_value_t result;
      double row_payoffs[] = {0.0, 11.0, -22.0, 3.0};
      double col_payoffs[] = {2.0, 1.0, -2.0, -3.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(2.0);

      result = toad_nash_repeated_game(5, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      check(fabs((double)(toad_map_get(&result, "rows").data.number) - (double)(32.0)) <= (double)(1e-9));
      check(fabs((double)(toad_map_get(&result, "cols").data.number) - (double)(32.0)) <= (double)(1e-9));
      check(fabs((double)(toad_map_get(&result, "states").data.number) - (double)(5.0)) <= (double)(1e-9));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("moran process") {
    it("returns a single generation for an already fixed population") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[6];
      toad_value_t result;
      toad_value_t history;
      toad_value_t population_items[3];
      double payoffs[] = {
        4.0, 3.0, 2.0,
        1.0, 2.0, 5.0,
        6.0, 1.0, 3.0
      };

      mem_init(&arena, 8192);
      toad_env_init(&env);

      population_items[0] = toad_val_num(0.0);
      population_items[1] = toad_val_num(0.0);
      population_items[2] = toad_val_num(0.0);

      args[0] = toad_val_vec(payoffs, 9);
      args[1] = toad_val_num(3.0);
      args[2] = toad_val_list_ex(population_items, 3, 0);
      args[3] = toad_val_num(10.0);
      args[4] = toad_val_num(0.0);
      args[5] = toad_val_num(0.0);

      result = toad_nash_moran_process(6, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      check(fabs((double)(toad_map_get(&result, "steps").data.number) - (double)(1.0)) <= (double)(1e-9));
      history = toad_map_get(&result, "history");
      check((history.type) == (TOAD_VAL_LIST));
      check(((int)history.data.list.count) == (1));
      check(fabs((double)(history.data.list.items[0].data.list.items[0].data.number) - (double)(0.0)) <= (double)(1e-9));
      check(fabs((double)(history.data.list.items[0].data.list.items[1].data.number) - (double)(0.0)) <= (double)(1e-9));
      check(fabs((double)(history.data.list.items[0].data.list.items[2].data.number) - (double)(0.0)) <= (double)(1e-9));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("respects an identity replacement graph") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[8];
      toad_value_t result;
      toad_value_t history;
      toad_value_t population_items[3];
      double payoffs[] = {
        4.0, 3.0, 2.0,
        1.0, 2.0, 5.0,
        6.0, 1.0, 3.0
      };
      double replacement[] = {
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
      };

      mem_init(&arena, 8192);
      toad_env_init(&env);

      population_items[0] = toad_val_num(0.0);
      population_items[1] = toad_val_num(1.0);
      population_items[2] = toad_val_num(2.0);

      args[0] = toad_val_vec(payoffs, 9);
      args[1] = toad_val_num(3.0);
      args[2] = toad_val_list_ex(population_items, 3, 0);
      args[3] = toad_val_num(5.0);
      args[4] = toad_val_num(0.0);
      args[5] = toad_val_num(3.0);
      args[6] = toad_val_vec(replacement, 9);
      args[7] = toad_val_num(0.0);

      result = toad_nash_moran_process(7, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      history = toad_map_get(&result, "history");
      for (size_t i = 0; i < history.data.list.count; i++) {
        check(fabs((double)(history.data.list.items[i].data.list.items[0].data.number) - (double)(0.0)) <= (double)(1e-9));
        check(fabs((double)(history.data.list.items[i].data.list.items[1].data.number) - (double)(1.0)) <= (double)(1e-9));
        check(fabs((double)(history.data.list.items[i].data.list.items[2].data.number) - (double)(2.0)) <= (double)(1e-9));
      }

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("mutation dynamics") {
    it("keeps the matching-pennies mixed equilibrium fixed under identity mutation") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[6];
      toad_value_t result;
      toad_value_t strategy;
      double payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double mutation[] = {1.0, 0.0, 0.0, 1.0};
      double initial[] = {0.5, 0.5};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(payoffs, 4);
      args[1] = toad_val_num(2.0);
      args[2] = toad_val_num(200.0);
      args[3] = toad_val_num(0.02);
      args[4] = toad_val_vec(mutation, 4);
      args[5] = toad_val_vec(initial, 2);

      result = toad_nash_replicator_mutation(6, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      strategy = toad_map_get(&result, "strategy");
      check(fabs((double)(strategy.data.vector.data[0]) - (double)(0.5)) <= (double)(1e-6));
      check(fabs((double)(strategy.data.vector.data[1]) - (double)(0.5)) <= (double)(1e-6));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("asymmetric replicator") {
    it("has near-zero derivative at the Nashpy 3x2 mixed equilibrium") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[6];
      toad_value_t result;
      toad_value_t row_derivative;
      toad_value_t col_derivative;
      double row_payoffs[] = {
        3.0, 3.0,
        2.0, 5.0,
        0.0, 6.0
      };
      double col_payoffs[] = {
        3.0, 2.0,
        2.0, 6.0,
        3.0, 1.0
      };
      double row_state[] = {0.0, 1.0 / 3.0, 2.0 / 3.0};
      double col_state[] = {1.0 / 3.0, 2.0 / 3.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 6);
      args[1] = toad_val_vec(col_payoffs, 6);
      args[2] = toad_val_num(3.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_vec(row_state, 3);
      args[5] = toad_val_vec(col_state, 2);

      result = toad_nash_asymmetric_replicator_derivative(6, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      row_derivative = toad_map_get(&result, "row_derivative");
      col_derivative = toad_map_get(&result, "col_derivative");
      check(fabs((double)(row_derivative.data.vector.data[0]) - (double)(0.0)) <= (double)(1e-9));
      check(fabs((double)(row_derivative.data.vector.data[1]) - (double)(0.0)) <= (double)(1e-9));
      check(fabs((double)(row_derivative.data.vector.data[2]) - (double)(0.0)) <= (double)(1e-9));
      check(fabs((double)(col_derivative.data.vector.data[0]) - (double)(0.0)) <= (double)(1e-9));
      check(fabs((double)(col_derivative.data.vector.data[1]) - (double)(0.0)) <= (double)(1e-9));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }

    it("keeps the same mixed equilibrium stable under Euler integration") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[8];
      toad_value_t result;
      toad_value_t row_vec;
      toad_value_t col_vec;
      double row_payoffs[] = {
        3.0, 3.0,
        2.0, 5.0,
        0.0, 6.0
      };
      double col_payoffs[] = {
        3.0, 2.0,
        2.0, 6.0,
        3.0, 1.0
      };
      double row_state[] = {0.0, 1.0 / 3.0, 2.0 / 3.0};
      double col_state[] = {1.0 / 3.0, 2.0 / 3.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 6);
      args[1] = toad_val_vec(col_payoffs, 6);
      args[2] = toad_val_num(3.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(500.0);
      args[5] = toad_val_num(0.01);
      args[6] = toad_val_vec(row_state, 3);
      args[7] = toad_val_vec(col_state, 2);

      result = toad_nash_asymmetric_replicator(8, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));
      row_vec = toad_map_get(&result, "row_strategy");
      col_vec = toad_map_get(&result, "col_strategy");

      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(0.0)) <= (double)(1e-6));
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(1.0 / 3.0)) <= (double)(1e-4));
      check(fabs((double)(row_vec.data.vector.data[2]) - (double)(2.0 / 3.0)) <= (double)(1e-4));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(1.0 / 3.0)) <= (double)(1e-4));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(2.0 / 3.0)) <= (double)(1e-4));

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }

  group("replicator") {
    it("converges to matching pennies mixed strategy") {
      mem_pool_t arena;
      toad_env_t env;
      toad_value_t args[7];
      toad_value_t result;
      toad_value_t row_vec;
      toad_value_t col_vec;
      double row_payoffs[] = {1.0, -1.0, -1.0, 1.0};
      double col_payoffs[] = {-1.0, 1.0, 1.0, -1.0};

      mem_init(&arena, 8192);
      toad_env_init(&env);

      args[0] = toad_val_vec(row_payoffs, 4);
      args[1] = toad_val_vec(col_payoffs, 4);
      args[2] = toad_val_num(2.0);
      args[3] = toad_val_num(2.0);
      args[4] = toad_val_num(500.0);
      args[5] = toad_val_num(0.1);
      args[6] = toad_val_num(1e-6);

      result = toad_nash_replicator(7, args, &env, &arena);
      check((result.type) == (TOAD_VAL_MAP));

      row_vec = toad_map_get(&result, "row_strategy");
      col_vec = toad_map_get(&result, "col_strategy");
      check(fabs((double)(row_vec.data.vector.data[0]) - (double)(0.5)) <= (double)(EPSILON));
      check(fabs((double)(row_vec.data.vector.data[1]) - (double)(0.5)) <= (double)(EPSILON));
      check(fabs((double)(col_vec.data.vector.data[0]) - (double)(0.5)) <= (double)(EPSILON));
      check(fabs((double)(col_vec.data.vector.data[1]) - (double)(0.5)) <= (double)(EPSILON));
      check(toad_map_get(&result, "converged").data.number >= 1.0);

      toad_map_free(&result);
      toad_env_free(&env);
      mem_destroy(&arena);
    }
  }
}
