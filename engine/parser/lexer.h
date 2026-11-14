/**
 * @file lexer.h
 * @brief engine Lexer Definitions
 */

#ifndef TBS_LEXER_H
#define TBS_LEXER_H

#include <stddef.h>
#include <turboscript/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *input;
    const char *cursor;
    const char *limit;
    int line;
    int column;
    int last_token;
    char error[256];
} tbs_lexer_t;

void tbs_lexer_init(tbs_lexer_t *lexer, const char *input, size_t length);
int  tbs_lexer_next(tbs_lexer_t *lexer, tbs_token_t *token);

#ifdef __cplusplus
}
#endif

#endif /* TBS_LEXER_H */
