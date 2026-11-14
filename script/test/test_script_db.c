#include <toad/script.h>
#include <toad/class.h>
#include "tinytest.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef TOAD_TEST_SQLITE_DRIVER_PATH
#define TOAD_TEST_SQLITE_DRIVER_PATH ""
#endif

static int append_quoted_path(char *out, size_t capacity, const char *path) {
  size_t used = 0u;
  if (!out || capacity == 0u || !path) return 0;

  for (const char *p = path; *p; ++p) {
    if (*p == '\\' || *p == '"') {
      if (used + 2u >= capacity) return 0;
      out[used++] = '\\';
      out[used++] = *p;
    } else {
      if (used + 1u >= capacity) return 0;
      out[used++] = *p;
    }
  }
  if (used >= capacity) return 0;
  out[used] = '\0';
  return 1;
}

static int build_success_script(char *out, size_t capacity) {
  char path[2048];
  if (!append_quoted_path(
          path, sizeof(path), TOAD_TEST_SQLITE_DRIVER_PATH))
    return 0;
  return snprintf(
             out, capacity,
             "a=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
             "options:map {filename:\":memory:\"}});"
             "b=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
             "options:map {filename:\":memory:\"}});"
             "ca=db.close(a);"
             "cb=db.close(b);",
             path, path) > 0;
}

static int build_failure_script(char *out, size_t capacity,
                                const char *driver,
                                const char *module_path,
                                const char *options_source) {
  char path[2048];
  if (!append_quoted_path(path, sizeof(path), module_path)) return 0;
  return snprintf(
             out, capacity,
             "x=db.connect(map {driver:\"%s\",module_path:\"%s\"%s});",
             driver, path, options_source ? options_source : "") > 0;
}

static toad_script_ctx_t *new_db_ctx(void) {
  toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
  if (!ctx) return NULL;
  if (toad_script_load_plugin(ctx, "db") != 0) {
    toad_script_free(ctx);
    return NULL;
  }
  return ctx;
}

spec("Toad TurboDB ORM configuration") {
  it("uses explicit user config for two connections on one loaded driver") {
    char script[8192];
    toad_script_ctx_t *interp = new_db_ctx();
    toad_script_ctx_t *jit = new_db_ctx();

    check_not_null(interp);
    check_not_null(jit);
    check_true(build_success_script(script, sizeof(script)));

    check_equal(toad_script_run(interp, script), 0);
    check_equal(toad_script_run_jit(jit, script), 0);

    check(fabs(toad_script_get_num(interp, "a") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(interp, "b") - 2.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "a") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "b") - 2.0) <= 1e-9);
    check(fabs(toad_script_get_num(interp, "ca") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(interp, "cb") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "ca") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "cb") - 1.0) <= 1e-9);

    toad_script_free(jit);
    toad_script_free(interp);
  }

  it("fails when the explicit driver ID does not match the module manifest") {
    char script[4096];
    toad_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(build_failure_script(
        script, sizeof(script), "postgresql",
        TOAD_TEST_SQLITE_DRIVER_PATH,
        ",options:map {filename:\":memory:\"}"));

    check(toad_script_run(ctx, script) != 0);
    check_not_null(strstr(toad_script_get_error(ctx), "driver"));

    toad_script_free(ctx);
  }

  it("does not invent a default module path") {
    toad_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check(toad_script_run(
              ctx,
              "x=db.connect(map {driver:\"sqlite\","
              "options:map {filename:\":memory:\"}});") != 0);
    check_not_null(strstr(toad_script_get_error(ctx), "module_path"));

    toad_script_free(ctx);
  }

  it("passes backend-specific options through to TurboDB and fails fast") {
    char script[4096];
    toad_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(build_failure_script(
        script, sizeof(script), "sqlite",
        TOAD_TEST_SQLITE_DRIVER_PATH,
        ",options:map {}"));

    check(toad_script_run(ctx, script) != 0);
    check_not_null(strstr(toad_script_get_error(ctx), "filename"));

    toad_script_free(ctx);
  }

  it("executes DDL and parameterized commands through TurboDB CFlow") {
    char path[2048];
    char script[12288];
    toad_script_ctx_t *interp = new_db_ctx();
    toad_script_ctx_t *jit = new_db_ctx();

    check_not_null(interp);
    check_not_null(jit);
    check_true(append_quoted_path(
        path, sizeof(path), TOAD_TEST_SQLITE_DRIVER_PATH));
    check(snprintf(
              script, sizeof(script),
              "dbx=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
              "options:map {filename:\":memory:\"}});"
              "ddl=db.exec(dbx,\"CREATE TABLE person(id INTEGER,name TEXT);\");"
              "one=db.exec(dbx,"
              "\"INSERT INTO person(id,name) VALUES(?1,?2);\","
              "[7,\"Alice\"]);"
              "two=db.exec(dbx,"
              "\"INSERT INTO person(id,name) VALUES(?1,?2);\","
              "[8,\"Bob\"]);"
              "closed=db.close(dbx);",
              path) > 0);

    check_equal(toad_script_run(interp, script), 0);
    check_equal(toad_script_run_jit(jit, script), 0);

    check(fabs(toad_script_get_num(interp, "ddl") - 0.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "ddl") - 0.0) <= 1e-9);
    check(fabs(toad_script_get_num(interp, "one") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "one") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(interp, "two") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "two") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(interp, "closed") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "closed") - 1.0) <= 1e-9);

    toad_script_free(jit);
    toad_script_free(interp);
  }

  it("rejects non-scalar command parameters before demand") {
    char path[2048];
    char script[8192];
    toad_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(append_quoted_path(
        path, sizeof(path), TOAD_TEST_SQLITE_DRIVER_PATH));
    check(snprintf(
              script, sizeof(script),
              "dbx=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
              "options:map {filename:\":memory:\"}});"
              "bad=db.exec(dbx,\"CREATE TABLE t(v INTEGER);\","
              "[map {x:1}]);",
              path) > 0);

    check(toad_script_run(ctx, script) != 0);
    check_not_null(strstr(toad_script_get_error(ctx), "params must be"));

    toad_script_free(ctx);
  }

  it("surfaces backend command errors without materialized fallback") {
    char path[2048];
    char script[8192];
    toad_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(append_quoted_path(
        path, sizeof(path), TOAD_TEST_SQLITE_DRIVER_PATH));
    check(snprintf(
              script, sizeof(script),
              "dbx=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
              "options:map {filename:\":memory:\"}});"
              "bad=db.exec(dbx,\"INSERT INTO missing_table(v) VALUES(1);\");",
              path) > 0);

    check(toad_script_run(ctx, script) != 0);
    check_not_null(strstr(toad_script_get_error(ctx), "db.exec"));

    toad_script_free(ctx);
  }

  it("decodes typed RowClass results through TurboDB object CFlow") {
    char path[2048];
    char script[16384];
    toad_script_ctx_t *interp = new_db_ctx();
    toad_script_ctx_t *jit = new_db_ctx();
    toad_value_t interp_rows;
    toad_value_t jit_rows;
    toad_value_t field;

    check_not_null(interp);
    check_not_null(jit);
    check_true(append_quoted_path(
        path, sizeof(path), TOAD_TEST_SQLITE_DRIVER_PATH));
    check(snprintf(
              script, sizeof(script),
              "class Person { id: int64; name: string; score: double; };"
              "dbx=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
              "options:map {filename:\":memory:\"}});"
              "db.exec(dbx,\"CREATE TABLE person(id INTEGER,name TEXT,score REAL);\");"
              "db.exec(dbx,"
              "\"INSERT INTO person(id,name,score) VALUES(?1,?2,?3);\","
              "[1,\"Ada\",1.5]);"
              "db.exec(dbx,"
              "\"INSERT INTO person(id,name,score) VALUES(?1,?2,?3);\","
              "[2,\"Bob\",2.5]);"
              "rows=db.query(dbx,"
              "\"SELECT id,name,score FROM person ORDER BY id;\","
              "Person);"
              "closed=db.close(dbx);",
              path) > 0);

    check_equal(toad_script_run(interp, script), 0);
    check_equal(toad_script_run_jit(jit, script), 0);

    interp_rows = toad_env_get(&interp->env, "rows");
    jit_rows = toad_env_get(&jit->env, "rows");
    check_equal(interp_rows.type, TOAD_VAL_LIST);
    check_equal(jit_rows.type, TOAD_VAL_LIST);
    check_equal(interp_rows.data.list.count, (size_t)2u);
    check_equal(jit_rows.data.list.count, (size_t)2u);

    check_equal(interp_rows.data.list.items[0].type, TOAD_VAL_INSTANCE);
    check_equal(interp_rows.data.list.items[1].type, TOAD_VAL_INSTANCE);
    check_equal(jit_rows.data.list.items[0].type, TOAD_VAL_INSTANCE);
    check_equal(jit_rows.data.list.items[1].type, TOAD_VAL_INSTANCE);

    check_true(toad_instance_get_field(
        interp_rows.data.list.items[0].data.instance_val.instance,
        "id", &field));
    check_equal(field.type, TOAD_VAL_INTEGER);
    check_equal(field.data.integer, INT64_C(1));

    check_true(toad_instance_get_field(
        interp_rows.data.list.items[0].data.instance_val.instance,
        "name", &field));
    check_equal(field.type, TOAD_VAL_STRING);
    check_equal(field.data.string.len, (size_t)3u);
    check(memcmp(field.data.string.data, "Ada", 3u) == 0);

    check_true(toad_instance_get_field(
        interp_rows.data.list.items[1].data.instance_val.instance,
        "score", &field));
    check_equal(field.type, TOAD_VAL_NUMBER);
    check(fabs(field.data.number - 2.5) <= 1e-9);

    check_true(toad_instance_get_field(
        jit_rows.data.list.items[0].data.instance_val.instance,
        "id", &field));
    check_equal(field.type, TOAD_VAL_INTEGER);
    check_equal(field.data.integer, INT64_C(1));

    check_true(toad_instance_get_field(
        jit_rows.data.list.items[1].data.instance_val.instance,
        "name", &field));
    check_equal(field.type, TOAD_VAL_STRING);
    check_equal(field.data.string.len, (size_t)3u);
    check(memcmp(field.data.string.data, "Bob", 3u) == 0);

    check(fabs(toad_script_get_num(interp, "closed") - 1.0) <= 1e-9);
    check(fabs(toad_script_get_num(jit, "closed") - 1.0) <= 1e-9);

    toad_script_free(jit);
    toad_script_free(interp);
  }

  it("binds typed query parameters before opening object flow") {
    char path[2048];
    char script[12288];
    toad_script_ctx_t *ctx = new_db_ctx();
    toad_value_t rows;
    toad_value_t field;

    check_not_null(ctx);
    check_true(append_quoted_path(
        path, sizeof(path), TOAD_TEST_SQLITE_DRIVER_PATH));
    check(snprintf(
              script, sizeof(script),
              "class Person { id: int64; name: string; score: double; };"
              "dbx=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
              "options:map {filename:\":memory:\"}});"
              "db.exec(dbx,\"CREATE TABLE person(id INTEGER,name TEXT,score REAL);\");"
              "db.exec(dbx,"
              "\"INSERT INTO person(id,name,score) VALUES(?1,?2,?3);\","
              "[1,\"Ada\",1.5]);"
              "db.exec(dbx,"
              "\"INSERT INTO person(id,name,score) VALUES(?1,?2,?3);\","
              "[2,\"Bob\",2.5]);"
              "rows=db.query(dbx,"
              "\"SELECT id,name,score FROM person WHERE id > ?1 ORDER BY id;\","
              "Person,[1]);",
              path) > 0);

    check_equal(toad_script_run(ctx, script), 0);
    rows = toad_env_get(&ctx->env, "rows");
    check_equal(rows.type, TOAD_VAL_LIST);
    check_equal(rows.data.list.count, (size_t)1u);
    check_equal(rows.data.list.items[0].type, TOAD_VAL_INSTANCE);
    check_true(toad_instance_get_field(
        rows.data.list.items[0].data.instance_val.instance,
        "name", &field));
    check_equal(field.type, TOAD_VAL_STRING);
    check_equal(field.data.string.len, (size_t)3u);
    check(memcmp(field.data.string.data, "Bob", 3u) == 0);

    toad_script_free(ctx);
  }

  it("surfaces DataBind field diagnostics for incompatible typed rows") {
    char path[2048];
    char script[12288];
    toad_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(append_quoted_path(
        path, sizeof(path), TOAD_TEST_SQLITE_DRIVER_PATH));
    check(snprintf(
              script, sizeof(script),
              "class Person { id: int64; name: string; score: double; };"
              "dbx=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
              "options:map {filename:\":memory:\"}});"
              "db.exec(dbx,\"CREATE TABLE person(id INTEGER,name TEXT,score REAL);\");"
              "db.exec(dbx,"
              "\"INSERT INTO person(id,name,score) VALUES(1,'Ada',1.5);\");"
              "rows=db.query(dbx,"
              "\"SELECT id,score FROM person;\","
              "Person);",
              path) > 0);

    check(toad_script_run(ctx, script) != 0);
    check_not_null(strstr(toad_script_get_error(ctx), "db.query failed"));
    check_not_null(strstr(toad_script_get_error(ctx), "name"));

    toad_script_free(ctx);
  }

  it("rejects changing module_path for an already loaded driver ID") {
    char path[2048];
    char script[8192];
    toad_script_ctx_t *ctx = new_db_ctx();

    check_not_null(ctx);
    check_true(append_quoted_path(
        path, sizeof(path), TOAD_TEST_SQLITE_DRIVER_PATH));
    check(snprintf(
              script, sizeof(script),
              "a=db.connect(map {driver:\"sqlite\",module_path:\"%s\","
              "options:map {filename:\":memory:\"}});"
              "b=db.connect(map {driver:\"sqlite\","
              "module_path:\"/definitely/different/driver\","
              "options:map {filename:\":memory:\"}});",
              path) > 0);

    check(toad_script_run(ctx, script) != 0);
    check_not_null(strstr(
        toad_script_get_error(ctx), "different module_path"));

    toad_script_free(ctx);
  }
}
