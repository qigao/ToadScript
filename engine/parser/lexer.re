// re2c $INPUT -o $OUTPUT
/**
 * @file lexer.re
 * @brief engine Lexer using re2c
 */

#include "lexer.h"
#include <turboscript/detail/grammar_gen.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static double parse_number(const char *start, const char *end) {
    char buf[64];
    size_t len = (size_t)(end - start);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';
    return strtod(buf, NULL);
}

static int regex_can_start_after(int token_type) {
    switch (token_type) {
        case 0:
        case tbs_TOKEN_EQUAL:
        case tbs_TOKEN_ASSIGN_ADD:
        case tbs_TOKEN_ASSIGN_SUB:
        case tbs_TOKEN_ASSIGN_MUL:
        case tbs_TOKEN_ASSIGN_DIV:
        case tbs_TOKEN_LPAREN:
        case tbs_TOKEN_LBRACKET:
        case tbs_TOKEN_LBRACE:
        case tbs_TOKEN_COMMA:
        case tbs_TOKEN_COLON:
        case tbs_TOKEN_SEMICOLON:
        case tbs_TOKEN_RETURN:
        case tbs_TOKEN_THROW:
        case tbs_TOKEN_CASE:
        case tbs_TOKEN_QUESTION:
        case tbs_TOKEN_ARROW:
        case tbs_TOKEN_PLUS:
        case tbs_TOKEN_MINUS:
        case tbs_TOKEN_MULTIPLY:
        case tbs_TOKEN_DIVIDE:
        case tbs_TOKEN_MOD:
        case tbs_TOKEN_POWER:
        case tbs_TOKEN_EQ:
        case tbs_TOKEN_NE:
        case tbs_TOKEN_LT:
        case tbs_TOKEN_LE:
        case tbs_TOKEN_GT:
        case tbs_TOKEN_GE:
        case tbs_TOKEN_AND:
        case tbs_TOKEN_OR:
        case tbs_TOKEN_NOT:
        case tbs_TOKEN_PIPE:
            return 1;
        default:
            return 0;
    }
}

static int scan_regex_literal(const char *start, const char *limit, const char **end) {
    const char *p = start + 1;
    int escaped = 0;
    int in_class = 0;

    while (p < limit) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\r') return -1;
        if (escaped) {
            escaped = 0;
            ++p;
            continue;
        }
        if (c == '\\') {
            escaped = 1;
            ++p;
            continue;
        }
        if (c == '[') {
            in_class = 1;
            ++p;
            continue;
        }
        if (c == ']' && in_class) {
            in_class = 0;
            ++p;
            continue;
        }
        if (c == '/' && !in_class) {
            ++p;
            while (p < limit && isalpha((unsigned char)*p)) ++p;
            *end = p;
            return 1;
        }
        ++p;
    }
    return -1;
}

static void lexer_note_token(tbs_lexer_t *lexer, int token_type) {
    lexer->last_token = token_type;
}

int tbs_lexer_next(tbs_lexer_t *lexer, tbs_token_t *token) {
    if (!lexer || !token) return -1;

    const char *YYCURSOR = lexer->cursor;
    const char *YYMARKER;
    const char *YYLIMIT = lexer->limit;
    const char *token_start;

    token->type = 0;

    #define RET(t) do { \
        token->type = t; \
        token->start = token_start; \
        token->length = (size_t)(YYCURSOR - token_start); \
        lexer->cursor = YYCURSOR; \
        lexer->column += (int)(YYCURSOR - token_start); \
        lexer_note_token(lexer, t); \
        return t; \
    } while(0)

lex_start:
    token_start = YYCURSOR;
    token->line = lexer->line;
    token->column = lexer->column;

    /*!re2c
        re2c:define:YYCTYPE = "unsigned char";
        re2c:yyfill:enable = 0;
        re2c:eof = 0;

        digit   = [0-9];
        int     = "0" | [1-9] digit*;
        frac    = "." digit+;
        exp     = [eE] [+-]? digit+;
        number  = int frac? exp?;

        ident   = [a-zA-Z_] [a-zA-Z0-9_]*;

        // EOF
        $ {
            token->type = 0;
            token->line = lexer->line;
            token->column = lexer->column;
            lexer->cursor = YYCURSOR;
            return 0;
        }

        // Operators
        "+" { RET(tbs_TOKEN_PLUS); }
        "-" { RET(tbs_TOKEN_MINUS); }
        "*" { RET(tbs_TOKEN_MULTIPLY); }
        "/" {
            if (regex_can_start_after(lexer->last_token)) {
                const char *regex_end = NULL;
                int regex_scan = scan_regex_literal(token_start, YYLIMIT, &regex_end);
                if (regex_scan > 0) {
                    YYCURSOR = regex_end;
                    RET(tbs_TOKEN_REGEX);
                }
                if (regex_scan < 0) {
                    snprintf(lexer->error, sizeof(lexer->error),
                             "Unterminated regex literal at line %d", lexer->line);
                    return -1;
                }
            }
            RET(tbs_TOKEN_DIVIDE);
        }
        "%" { RET(tbs_TOKEN_MOD); }
        "^" { RET(tbs_TOKEN_POWER); }
        "..." { RET(tbs_TOKEN_SPREAD); }
        ".." { RET(tbs_TOKEN_DOTDOT); }
        "."  { RET(tbs_TOKEN_DOT); }
        "?." { RET(tbs_TOKEN_QUESTION_DOT); }
        "?"  { RET(tbs_TOKEN_QUESTION); }

        // Comparison
        "==" { RET(tbs_TOKEN_EQ); }
        "=>" { RET(tbs_TOKEN_ARROW); }
        "="  { RET(tbs_TOKEN_EQUAL); }
        "!=" { RET(tbs_TOKEN_NE); }
        "<>" { RET(tbs_TOKEN_NE); }
        "<"  { RET(tbs_TOKEN_LT); }
        "<=" { RET(tbs_TOKEN_LE); }
        ">"  { RET(tbs_TOKEN_GT); }
        ">=" { RET(tbs_TOKEN_GE); }

        // Assignment

        "+=" { RET(tbs_TOKEN_ASSIGN_ADD); }
        "-=" { RET(tbs_TOKEN_ASSIGN_SUB); }
        "*=" { RET(tbs_TOKEN_ASSIGN_MUL); }
        "/=" { RET(tbs_TOKEN_ASSIGN_DIV); }

        // Logic
        "and" { RET(tbs_TOKEN_AND); }
        "or"  { RET(tbs_TOKEN_OR); }
        "not" { RET(tbs_TOKEN_NOT); }
        "|>"  { RET(tbs_TOKEN_PIPE); }
        "&&"  { RET(tbs_TOKEN_AND); }
        "||"  { RET(tbs_TOKEN_OR); }
        "!"   { RET(tbs_TOKEN_NOT); }

        // Control Flow
        "if"       { RET(tbs_TOKEN_IF); }
        "elif"     { RET(tbs_TOKEN_ELIF); }
        "else"     { RET(tbs_TOKEN_ELSE); }
        "while"    { RET(tbs_TOKEN_WHILE); }
        "do"       { RET(tbs_TOKEN_DO); }
        "for"      { RET(tbs_TOKEN_FOR); }
        "break"    { RET(tbs_TOKEN_BREAK); }
        "continue" { RET(tbs_TOKEN_CONTINUE); }
        "return"   { RET(tbs_TOKEN_RETURN); }
        "var"      { RET(tbs_TOKEN_VAR); }
        "let"      { RET(tbs_TOKEN_VAR); }
        "const"    { RET(tbs_TOKEN_CONST); }

        // OOP Keywords
        "class"       { RET(tbs_TOKEN_CLASS); }
        "constructor" { RET(tbs_TOKEN_CONSTRUCTOR); }
        "this"        { RET(tbs_TOKEN_THIS); }
        "static"      { RET(tbs_TOKEN_STATIC); }
        "extends"     { RET(tbs_TOKEN_EXTENDS); }
        "implements"  { RET(tbs_TOKEN_IMPLEMENTS); }
        "interface"   { RET(tbs_TOKEN_INTERFACE); }
        "super"       { RET(tbs_TOKEN_SUPER); }
        "new"         { RET(tbs_TOKEN_NEW); }
        "instanceof"  { RET(tbs_TOKEN_INSTANCEOF); }
        "abstract"    { RET(tbs_TOKEN_ABSTRACT); }
        "override"    { RET(tbs_TOKEN_OVERRIDE); }
        "final"       { RET(tbs_TOKEN_FINAL); }
        "public"      { RET(tbs_TOKEN_PUBLIC); }
        "protected"   { RET(tbs_TOKEN_PROTECTED); }
        "private"     { RET(tbs_TOKEN_PRIVATE); }
        "async"       { RET(tbs_TOKEN_ASYNC); }
        "await"       { RET(tbs_TOKEN_AWAIT); }
        // TODO: 协程语法暂时禁用
        // "function*" { RET(tbs_TOKEN_FUNC_GENERATOR); }
        // "func*"    { RET(tbs_TOKEN_FUNC_GENERATOR); }
        "function" { RET(tbs_TOKEN_FUNC); }
        "func"     { RET(tbs_TOKEN_FUNC); }
        "map"      { RET(tbs_TOKEN_MAP); }
        "struct"   { RET(tbs_TOKEN_MAP); }
        "in"       { RET(tbs_TOKEN_IN); }
        "null"     { RET(tbs_TOKEN_NULL); }
        "nil"      { RET(tbs_TOKEN_NULL); }
        "switch"   { RET(tbs_TOKEN_SWITCH); }
        "case"     { RET(tbs_TOKEN_CASE); }
        "default"  { RET(tbs_TOKEN_DEFAULT); }
        "try"      { RET(tbs_TOKEN_TRY); }
        "catch"    { RET(tbs_TOKEN_CATCH); }
        "throw"    { RET(tbs_TOKEN_THROW); }
        // TODO: 协程语法暂时禁用
        // "yield"    { RET(tbs_TOKEN_YIELD); }

        // Structure
        "{" { RET(tbs_TOKEN_LBRACE); }
        "}" { RET(tbs_TOKEN_RBRACE); }
        ";" { RET(tbs_TOKEN_SEMICOLON); }
        ":" { RET(tbs_TOKEN_COLON); }
        "..." { RET(tbs_TOKEN_SPREAD); }

        // Punctuation
        "(" { RET(tbs_TOKEN_LPAREN); }
        ")" { RET(tbs_TOKEN_RPAREN); }
        "[" { RET(tbs_TOKEN_LBRACKET); }
        "]" { RET(tbs_TOKEN_RBRACKET); }
        "," { RET(tbs_TOKEN_COMMA); }

        "\"" ([^"\\] | "\\" .)* "\"" {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            RET(tbs_TOKEN_STRING);
        }

        "`" ([^`\\] | "\\" .)* "`" {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            RET(tbs_TOKEN_TEMPLATE);
        }

        "//" [^\n]* {
            lexer->cursor = YYCURSOR;
            goto lex_start;
        }

        "/*" {
             while (YYCURSOR < YYLIMIT) {
                 if (*YYCURSOR == '\n') { lexer->line++; lexer->column = 1; }
                 else { lexer->column++; }
                 if (*YYCURSOR == '*' && YYCURSOR + 1 < YYLIMIT && YYCURSOR[1] == '/') {
                     YYCURSOR += 2; lexer->column += 2; break;
                 }
                 YYCURSOR++;
             }
             lexer->cursor = YYCURSOR;
             goto lex_start;
        }

        "\n" {
            lexer->line++;
            lexer->column = 1;
            lexer->cursor = YYCURSOR;
            goto lex_start;
        }

        [ \t\r]+ {
            lexer->column += (int)(YYCURSOR - token_start);
            lexer->cursor = YYCURSOR;
            goto lex_start;
        }

        number {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            token->line = lexer->line;
            token->column = lexer->column;

            // Check if it's an integer (no '.' or 'e'/'E')
            int is_integer = 1;
            for (const char *p = token_start; p < YYCURSOR; ++p) {
                if (*p == '.' || *p == 'e' || *p == 'E') {
                    is_integer = 0;
                    break;
                }
            }

            if (is_integer) {
                token->type = tbs_TOKEN_INTEGER;
                char buf[32];
                size_t len = token->length;
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memcpy(buf, token_start, len);
                buf[len] = '\0';
                token->num_value = (double)strtoll(buf, NULL, 10);
            } else {
                token->type = tbs_TOKEN_NUMBER;
                token->num_value = parse_number(token_start, YYCURSOR);
            }

            lexer->column += (int)token->length;
            lexer->cursor = YYCURSOR;
            lexer_note_token(lexer, token->type);
            return token->type;
        }

        ident {
            token->type = tbs_TOKEN_VARIABLE;
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            token->line = lexer->line;
            token->column = lexer->column;
            lexer->column += (int)token->length;
            lexer->cursor = YYCURSOR;
            lexer_note_token(lexer, tbs_TOKEN_VARIABLE);
            return tbs_TOKEN_VARIABLE;
        }

        * {
            // Unexpected
            // token->type = tbs_TOKEN_ERROR;
            // lexer->cursor = YYCURSOR;
            // return -1;
            // Skip unknown chars for now or error?
            // Let's error
            return -1;
        }
    */
}

void tbs_lexer_init(tbs_lexer_t *lexer, const char *input, size_t length) {
    if (!lexer) return;
    lexer->input = input;
    lexer->cursor = input;
    lexer->limit = input + length;
    lexer->line = 1;
    lexer->column = 1;
    lexer->last_token = 0;
    lexer->error[0] = '\0';
}
