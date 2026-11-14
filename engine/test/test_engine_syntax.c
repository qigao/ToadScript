#include "tinytest.h"

#include <string.h>

#include <turboscript/engine.h>

suite("Engine Syntax") {
  group("parse lifecycle") {
    it("parses and frees a valid syntax tree without an evaluator") {
      const char *source = "answer = 6 * 7;";
      tbs_node_t *root = tbs_parse(source, strlen(source));

      check_not_null(root);
      tbs_free(root);
    }
  }
}
