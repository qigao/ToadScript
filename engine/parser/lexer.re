// re2c $INPUT -o $OUTPUT
/**
 * @file lexer.re
 * @brief engine Lexer using re2c
 */

#include "lexer.h"
#include <toad/detail/grammar_gen.h>
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
        case toad_TOKEN_EQUAL:
        case toad_TOKEN_ASSIGN_ADD:
        case toad_TOKEN_ASSIGN_SUB:
        case toad_TOKEN_ASSIGN_MUL:
        case toad_TOKEN_ASSIGN_DIV:
        case toad_TOKEN_LPAREN:
        case toad_TOKEN_LBRACKET:
        case toad_TOKEN_LBRACE:
        case toad_TOKEN_COMMA:
        case toad_TOKEN_COLON:
        case toad_TOKEN_SEMICOLON:
        case toad_TOKEN_RETURN:
        case toad_TOKEN_THROW:
        case toad_TOKEN_CASE:
        case toad_TOKEN_QUESTION:
        case toad_TOKEN_ARROW:
        case toad_TOKEN_PLUS:
        case toad_TOKEN_MINUS:
        case toad_TOKEN_MULTIPLY:
        case toad_TOKEN_DIVIDE:
        case toad_TOKEN_MOD:
        case toad_TOKEN_POWER:
        case toad_TOKEN_EQ:
        case toad_TOKEN_NE:
        case toad_TOKEN_LT:
        case toad_TOKEN_LE:
        case toad_TOKEN_GT:
        case toad_TOKEN_GE:
        case toad_TOKEN_AND:
        case toad_TOKEN_OR:
        case toad_TOKEN_NOT:
        case toad_TOKEN_PIPE:
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

static void lexer_note_token(toad_lexer_t *lexer, int token_type) {
    lexer->last_token = token_type;
}

int toad_lexer_next(toad_lexer_t *lexer, toad_token_t *token) {
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
        "+" { RET(toad_TOKEN_PLUS); }
        "-" { RET(toad_TOKEN_MINUS); }
        "*" { RET(toad_TOKEN_MULTIPLY); }
        "/" {
            if (regex_can_start_after(lexer->last_token)) {
                const char *regex_end = NULL;
                int regex_scan = scan_regex_literal(token_start, YYLIMIT, &regex_end);
                if (regex_scan > 0) {
                    YYCURSOR = regex_end;
                    RET(toad_TOKEN_REGEX);
                }
                if (regex_scan < 0) {
                    snprintf(lexer->error, sizeof(lexer->error),
                             "Unterminated regex literal at line %d", lexer->line);
                    return -1;
                }
            }
            RET(toad_TOKEN_DIVIDE);
        }
        "%" { RET(toad_TOKEN_MOD); }
        "^" { RET(toad_TOKEN_POWER); }
        "..." { RET(toad_TOKEN_SPREAD); }
        ".." { RET(toad_TOKEN_DOTDOT); }
        "."  { RET(toad_TOKEN_DOT); }
        "?." { RET(toad_TOKEN_QUESTION_DOT); }
        "?"  { RET(toad_TOKEN_QUESTION); }

        // Comparison
        "==" { RET(toad_TOKEN_EQ); }
        "=>" { RET(toad_TOKEN_ARROW); }
        "="  { RET(toad_TOKEN_EQUAL); }
        "!=" { RET(toad_TOKEN_NE); }
        "<>" { RET(toad_TOKEN_NE); }
        "<"  { RET(toad_TOKEN_LT); }
        "<=" { RET(toad_TOKEN_LE); }
        ">"  { RET(toad_TOKEN_GT); }
        ">=" { RET(toad_TOKEN_GE); }

        // Assignment

        "+=" { RET(toad_TOKEN_ASSIGN_ADD); }
        "-=" { RET(toad_TOKEN_ASSIGN_SUB); }
        "*=" { RET(toad_TOKEN_ASSIGN_MUL); }
        "/=" { RET(toad_TOKEN_ASSIGN_DIV); }

        // Logic
        "and" { RET(toad_TOKEN_AND); }
        "or"  { RET(toad_TOKEN_OR); }
        "not" { RET(toad_TOKEN_NOT); }
        "|>"  { RET(toad_TOKEN_PIPE); }
        "&&"  { RET(toad_TOKEN_AND); }
        "||"  { RET(toad_TOKEN_OR); }
        "!"   { RET(toad_TOKEN_NOT); }

        // Control Flow
        "if"       { RET(toad_TOKEN_IF); }
        "elif"     { RET(toad_TOKEN_ELIF); }
        "else"     { RET(toad_TOKEN_ELSE); }
        "while"    { RET(toad_TOKEN_WHILE); }
        "do"       { RET(toad_TOKEN_DO); }
        "for"      { RET(toad_TOKEN_FOR); }
        "break"    { RET(toad_TOKEN_BREAK); }
        "continue" { RET(toad_TOKEN_CONTINUE); }
        "return"   { RET(toad_TOKEN_RETURN); }
        "var"      { RET(toad_TOKEN_VAR); }
        "let"      { RET(toad_TOKEN_VAR); }
        "const"    { RET(toad_TOKEN_CONST); }

        // OOP Keywords
        "class"       { RET(toad_TOKEN_CLASS); }
        "constructor" { RET(toad_TOKEN_CONSTRUCTOR); }
        "this"        { RET(toad_TOKEN_THIS); }
        "static"      { RET(toad_TOKEN_STATIC); }
        "extends"     { RET(toad_TOKEN_EXTENDS); }
        "implements"  { RET(toad_TOKEN_IMPLEMENTS); }
        "interface"   { RET(toad_TOKEN_INTERFACE); }
        "super"       { RET(toad_TOKEN_SUPER); }
        "new"         { RET(toad_TOKEN_NEW); }
        "instanceof"  { RET(toad_TOKEN_INSTANCEOF); }
        "abstract"    { RET(toad_TOKEN_ABSTRACT); }
        "override"    { RET(toad_TOKEN_OVERRIDE); }
        "final"       { RET(toad_TOKEN_FINAL); }
        "public"      { RET(toad_TOKEN_PUBLIC); }
        "protected"   { RET(toad_TOKEN_PROTECTED); }
        "private"     { RET(toad_TOKEN_PRIVATE); }
        "async"       { RET(toad_TOKEN_ASYNC); }
        "await"       { RET(toad_TOKEN_AWAIT); }
        // TODO: 协程语法暂时禁用
        // "function*" { RET(toad_TOKEN_FUNC_GENERATOR); }
        // "func*"    { RET(toad_TOKEN_FUNC_GENERATOR); }
        "function" { RET(toad_TOKEN_FUNC); }
        "func"     { RET(toad_TOKEN_FUNC); }
        "map"      { RET(toad_TOKEN_MAP); }
        "struct"   { RET(toad_TOKEN_MAP); }
        "in"       { RET(toad_TOKEN_IN); }
        "null"     { RET(toad_TOKEN_NULL); }
        "nil"      { RET(toad_TOKEN_NULL); }
        "switch"   { RET(toad_TOKEN_SWITCH); }
        "case"     { RET(toad_TOKEN_CASE); }
        "default"  { RET(toad_TOKEN_DEFAULT); }
        "try"      { RET(toad_TOKEN_TRY); }
        "catch"    { RET(toad_TOKEN_CATCH); }
        "throw"    { RET(toad_TOKEN_THROW); }
        // TODO: 协程语法暂时禁用
        // "yield"    { RET(toad_TOKEN_YIELD); }

        // Structure
        "{" { RET(toad_TOKEN_LBRACE); }
        "}" { RET(toad_TOKEN_RBRACE); }
        ";" { RET(toad_TOKEN_SEMICOLON); }
        ":" { RET(toad_TOKEN_COLON); }
        "..." { RET(toad_TOKEN_SPREAD); }

        // Punctuation
        "(" { RET(toad_TOKEN_LPAREN); }
        ")" { RET(toad_TOKEN_RPAREN); }
        "[" { RET(toad_TOKEN_LBRACKET); }
        "]" { RET(toad_TOKEN_RBRACKET); }
        "," { RET(toad_TOKEN_COMMA); }

        "\"" ([^"\\] | "\\" .)* "\"" {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            RET(toad_TOKEN_STRING);
        }

        "`" ([^`\\] | "\\" .)* "`" {
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            RET(toad_TOKEN_TEMPLATE);
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
                token->type = toad_TOKEN_INTEGER;
                char buf[32];
                size_t len = token->length;
                if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                memcpy(buf, token_start, len);
                buf[len] = '\0';
                token->num_value = (double)strtoll(buf, NULL, 10);
            } else {
                token->type = toad_TOKEN_NUMBER;
                token->num_value = parse_number(token_start, YYCURSOR);
            }

            lexer->column += (int)token->length;
            lexer->cursor = YYCURSOR;
            lexer_note_token(lexer, token->type);
            return token->type;
        }

        ident {
            token->type = toad_TOKEN_VARIABLE;
            token->start = token_start;
            token->length = (size_t)(YYCURSOR - token_start);
            token->line = lexer->line;
            token->column = lexer->column;
            lexer->column += (int)token->length;
            lexer->cursor = YYCURSOR;
            lexer_note_token(lexer, toad_TOKEN_VARIABLE);
            return toad_TOKEN_VARIABLE;
        }

        * {
            // Unexpected
            // token->type = toad_TOKEN_ERROR;
            // lexer->cursor = YYCURSOR;
            // return -1;
            // Skip unknown chars for now or error?
            // Let's error
            return -1;
        }
    */
}

void toad_lexer_init(toad_lexer_t *lexer, const char *input, size_t length) {
    if (!lexer) return;
    lexer->input = input;
    lexer->cursor = input;
    lexer->limit = input + length;
    lexer->line = 1;
    lexer->column = 1;
    lexer->last_token = 0;
    lexer->error[0] = '\0';
}
