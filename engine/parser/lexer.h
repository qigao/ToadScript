/**
 * @file lexer.h
 * @brief engine Lexer Definitions
 */

#ifndef TOAD_LEXER_H
#define TOAD_LEXER_H

#include <stddef.h>
#include <toad/types.h>

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
} toad_lexer_t;

void toad_lexer_init(toad_lexer_t *lexer, const char *input, size_t length);
int  toad_lexer_next(toad_lexer_t *lexer, toad_token_t *token);

#ifdef __cplusplus
}
#endif

#endif /* TOAD_LEXER_H */
