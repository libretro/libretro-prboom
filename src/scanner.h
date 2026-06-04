/* scanner.h -- MSVC C89 port of Blzut3's UDMF tokenizer.
 *
 * Original (C) 2010 Braden "Blzut3" Obrzut, BSD-style license (retained in
 * scanner.c).  Ported from the C++ class form to a plain C89 struct + free
 * functions for use in the prboom/libretro renderer, which builds under
 * -std=c89 -pedantic -Werror=declaration-after-statement and MSVC C89.  No
 * C99/C11, no // comments, all declarations at block tops, manual memory.
 *
 * Usage:
 *   scanner_t s;
 *   scanner_init(&s, text, length);   // length < 0 => strlen
 *   while (scanner_tokens_left(&s)) {
 *     if (scanner_get_next_token(&s, TRUE)) { ... s.token / s.string ... }
 *   }
 *   scanner_free(&s);
 */

#ifndef __SCANNER_H__
#define __SCANNER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "custombool.h" /* dbool, TRUE/FALSE */

enum
{
  TK_Identifier,    /* SomeIdentifier   */
  TK_StringConst,   /* "Some String"    */
  TK_IntConst,      /* 27               */
  TK_FloatConst,    /* 1.5              */
  TK_BoolConst,     /* true             */
  TK_AndAnd,        /* &&               */
  TK_OrOr,          /* ||               */
  TK_EqEq,          /* ==               */
  TK_NotEq,         /* !=               */
  TK_GtrEq,         /* >=               */
  TK_LessEq,        /* <=               */
  TK_ShiftLeft,     /* <<               */
  TK_ShiftRight,    /* >>               */

  TK_NumSpecialTokens,

  TK_NoToken = -1
};

/* One token's worth of decoded state. */
typedef struct
{
  char         *string;
  int           number;
  double        decimal;
  dbool      boolean;
  char          token;
  unsigned int  tokenLine;
  unsigned int  tokenLinePosition;
} scanner_state_t;

typedef struct
{
  /* current (expanded) token -- the public fields callers read */
  char         *string;
  int           number;
  double        decimal;
  dbool      boolean;
  char          token;

  /* lookahead / bookkeeping */
  scanner_state_t nextState;

  char         *data;
  unsigned int  length;

  unsigned int  line;
  unsigned int  lineStart;
  unsigned int  logicalPosition;
  unsigned int  tokenLine;
  unsigned int  tokenLinePosition;
  unsigned int  scanPos;

  dbool      needNext;
} scanner_t;

/* lifecycle */
void scanner_init(scanner_t *s, const char *data, int length);
void scanner_free(scanner_t *s);

/* error callback (defaults to a stderr printer) */
void scanner_set_error_callback(void (*cb)(const char *msg, ...));

/* tokenizing */
dbool scanner_get_next_token(scanner_t *s, dbool expandState);
dbool scanner_check_token(scanner_t *s, char token);
dbool scanner_check_integer(scanner_t *s);
dbool scanner_check_float(scanner_t *s);
dbool scanner_check_string(scanner_t *s);
dbool scanner_string_match(scanner_t *s, const char *target);
void     scanner_must_get_integer(scanner_t *s);
void     scanner_must_get_float(scanner_t *s);
void     scanner_must_get_string(scanner_t *s);
void     scanner_must_get_token(scanner_t *s, char token);
void     scanner_must_get_identifier(scanner_t *s, const char *ident);
dbool scanner_tokens_left(scanner_t *s);
void     scanner_skip_line(scanner_t *s);
void     scanner_unget(scanner_t *s);
int      scanner_get_line(const scanner_t *s);
int      scanner_get_line_pos(const scanner_t *s);

#ifdef __cplusplus
}
#endif

#endif /* __SCANNER_H__ */
