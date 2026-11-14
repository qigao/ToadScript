#include <toad/script.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
  const char *eval = NULL, *file = NULL;
  toad_script_ctx_t *ctx;
  int rc;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--eval") && i + 1 < argc) eval = argv[++i];
    else if ((!strcmp(argv[i], "--file") || !strcmp(argv[i], "-f")) && i + 1 < argc) file = argv[++i];
    else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { puts("Usage: toad [--eval SCRIPT | --file PATH]"); return 0; }
    else { fprintf(stderr, "Unknown or incomplete option: %s\n", argv[i]); return 2; }
  }
  if ((eval && file) || (!eval && !file)) { fputs("Specify exactly one of --eval or --file.\n", stderr); return 2; }
  ctx = toad_script_init(TOAD_SCRIPT_INIT_DEFAULT);
  if (!ctx) { fputs("Failed to initialize Toad.\n", stderr); return 1; }
  rc = eval ? toad_script_run(ctx, eval) : toad_script_run_file(ctx, file);
  if (rc < 0) fprintf(stderr, "Toad: %s\n", toad_script_get_error(ctx));
  toad_script_free(ctx);
  return rc < 0 ? 1 : 0;
}
