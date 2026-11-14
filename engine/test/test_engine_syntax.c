#include "tinytest.h"

#include <string.h>

#include <toad/engine.h>

suite("Engine Syntax") {
  group("parse lifecycle") {
    it("parses and frees a valid syntax tree without an evaluator") {
      const char *source = "answer = 6 * 7;";
      toad_node_t *root = toad_parse(source, strlen(source));

      check_not_null(root);
      toad_free(root);
    }
  }
}
