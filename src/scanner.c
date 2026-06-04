/* scanner.c -- MSVC C89 port of Blzut3's UDMF tokenizer.
 *
 * Copyright (c) 2010, Braden "Blzut3" Obrzut <admin@maniacsvault.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    * Neither the name of the <organization> nor the
 *      names of its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.  See original for the
 * full text.  This file is a straight C89 translation of the C++ original;
 * behavior is intended to be identical.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "scanner.h"

#if defined(_WIN32) && !defined(__MINGW32__)
# define strcmpnocase  _stricmp
#else
# include <strings.h>
# define strcmpnocase  strcasecmp
#endif

static const char *const TokenNames[TK_NumSpecialTokens] =
{
  "Identifier",
  "String Constant",
  "Integer Constant",
  "Float Constant",
  "Boolean Constant",
  "Logical And",
  "Logical Or",
  "Equals",
  "Not Equals",
  "Greater Than or Equals",
  "Less Than or Equals",
  "Left Shift",
  "Right Shift"
};

static void scanner_standard_error(const char *message, ...)
{
  va_list list;
  va_start(list, message);
  vfprintf(stderr, message, list);
  va_end(list);
}

static void (*scanner_error_cb)(const char *message, ...) = scanner_standard_error;

/* C89/MSVC don't guarantee strdup; provide a local one. */
static char *strdup_local(const char *src)
{
  size_t n;
  char *d;
  if (src == NULL)
    return NULL;
  n = strlen(src) + 1;
  d = (char *)malloc(n);
  if (d)
    memcpy(d, src, n);
  return d;
}

/* Free the heap strings owned by a saved scanner state (its data pointer is
 * always NULL, so we never touch the live buffer). */
static void scanner_free_saved(scanner_t *saved)
{
  if (saved->string != NULL)           { free(saved->string);           saved->string = NULL; }
  if (saved->nextState.string != NULL) { free(saved->nextState.string); saved->nextState.string = NULL; }
}

void scanner_set_error_callback(void (*cb)(const char *msg, ...))
{
  scanner_error_cb = cb;
}

/* forward decls (block-top style; C89 needs these before first use) */
static void     scanner_set_string(char **ptr, const char *start, int length);
static void     scanner_check_for_whitespace(scanner_t *s);
static void     scanner_expand_state(scanner_t *s);
static void     scanner_increment_line(scanner_t *s);
static void     scanner_unescape(char *str);
static dbool scanner_scan_integer(scanner_t *s);
static dbool scanner_scan_float(scanner_t *s);
static void     scanner_save_state(scanner_t *s, scanner_t *saved);
static void     scanner_restore_state(scanner_t *s, scanner_t *saved);
static void     scanner_error_tok(scanner_t *s, int token);
static void     scanner_error_str(scanner_t *s, const char *mustget);
static void     scanner_errorf(scanner_t *s, const char *msg, ...);

void scanner_init(scanner_t *s, const char *data, int length)
{
  memset(s, 0, sizeof(*s));
  s->line          = 1;
  s->tokenLine     = 1;
  s->needNext      = TRUE;

  if (length < 0)
    length = (int)strlen(data);
  s->length = (unsigned int)length;
  s->data   = (char *)malloc((size_t)length ? (size_t)length : 1);
  if (length)
    memcpy(s->data, data, (size_t)length);
  s->string           = NULL;
  s->nextState.string = NULL;

  scanner_check_for_whitespace(s);
}

void scanner_free(scanner_t *s)
{
  if (s->string != NULL)           { free(s->string);           s->string = NULL; }
  if (s->nextState.string != NULL) { free(s->nextState.string); s->nextState.string = NULL; }
  if (s->data != NULL)             { free(s->data);             s->data = NULL; }
}

static void scanner_set_string(char **ptr, const char *start, int length)
{
  if (length < 0)
    length = (int)strlen(start);
  if (*ptr != NULL)
    free(*ptr);
  *ptr = (char *)malloc((size_t)length + 1);
  if (length)
    memcpy(*ptr, start, (size_t)length);
  (*ptr)[length] = 0;
}

static void scanner_increment_line(scanner_t *s)
{
  s->line++;
  s->lineStart = s->scanPos;
}

static void scanner_check_for_whitespace(scanner_t *s)
{
  int comment = 0; /* 1 = till next new line, 2 = till end block */
  while (s->scanPos < s->length)
  {
    char cur  = s->data[s->scanPos];
    char next = (s->scanPos + 1 < s->length) ? s->data[s->scanPos + 1] : 0;

    if (comment == 2)
    {
      if (cur != '*' || next != '/')
      {
        if (cur == '\n' || cur == '\r')
        {
          s->scanPos++;
          if (comment == 1)
            comment = 0;
          if (cur == '\r' && next == '\n')
            s->scanPos++;
          scanner_increment_line(s);
        }
        else
          s->scanPos++;
      }
      else
      {
        comment = 0;
        s->scanPos += 2;
      }
      continue;
    }

    if (cur == ' ' || cur == '\t' || cur == 0)
      s->scanPos++;
    else if (cur == '\n' || cur == '\r')
    {
      s->scanPos++;
      if (comment == 1)
        comment = 0;
      if (cur == '\r' && next == '\n')
        s->scanPos++;
      scanner_increment_line(s);
    }
    else if (cur == '/' && comment == 0)
    {
      switch (next)
      {
        case '/': comment = 1; break;
        case '*': comment = 2; break;
        default:  return;
      }
      s->scanPos += 2;
    }
    else
    {
      if (comment == 0)
        return;
      else
        s->scanPos++;
    }
  }
}

static void scanner_expand_state(scanner_t *s)
{
  s->logicalPosition = s->scanPos;
  scanner_check_for_whitespace(s);

  scanner_set_string(&s->string, s->nextState.string, -1);
  s->number            = s->nextState.number;
  s->decimal           = s->nextState.decimal;
  s->boolean           = s->nextState.boolean;
  s->token             = s->nextState.token;
  s->tokenLine         = s->nextState.tokenLine;
  s->tokenLinePosition = s->nextState.tokenLinePosition;
}

dbool scanner_check_token(scanner_t *s, char token)
{
  if (s->needNext)
  {
    if (!scanner_get_next_token(s, FALSE))
      return FALSE;
  }

  /* An int can also be a float. */
  if (s->nextState.token == token ||
      (s->nextState.token == TK_IntConst && token == TK_FloatConst))
  {
    s->needNext = TRUE;
    scanner_expand_state(s);
    return TRUE;
  }
  s->needNext = FALSE;
  return FALSE;
}

/* Deep-copy the scanner state (minus data pointer) for check/restore. */
static void scanner_save_state(scanner_t *s, scanner_t *saved)
{
  if (saved->string != NULL)           free(saved->string);
  if (saved->nextState.string != NULL) free(saved->nextState.string);
  *saved = *s;
  saved->string           = (s->string           ? strdup_local(s->string)           : NULL);
  saved->nextState.string = (s->nextState.string ? strdup_local(s->nextState.string) : NULL);
  saved->data = NULL;
}

static void scanner_restore_state(scanner_t *s, scanner_t *saved)
{
  char *kept_data = s->data;
  unsigned int kept_len = s->length;
  /* free the strings we are about to overwrite */
  if (s->string != NULL)           free(s->string);
  if (s->nextState.string != NULL) free(s->nextState.string);
  *s = *saved;
  s->data   = kept_data;   /* never clobbered by save (saved->data == NULL) */
  s->length = kept_len;
  /* saved owns its string copies; hand them to s and null them in saved so
   * scanner_free on saved won't double-free. */
  saved->string           = NULL;
  saved->nextState.string = NULL;
}

dbool scanner_get_next_token(scanner_t *s, dbool expandState)
{
  int start, end, integerBase;
  dbool floatHasDecimal, floatHasExponent, stringFinished;
  char cur;

  if (!s->needNext)
  {
    s->needNext = TRUE;
    if (expandState)
      scanner_expand_state(s);
    return TRUE;
  }

  s->nextState.tokenLine         = s->line;
  s->nextState.tokenLinePosition = s->scanPos - s->lineStart;
  s->nextState.token             = TK_NoToken;
  if (s->scanPos >= s->length)
  {
    if (expandState)
      scanner_expand_state(s);
    return FALSE;
  }

  start            = (int)s->scanPos;
  end              = (int)s->scanPos;
  integerBase      = 10;
  floatHasDecimal  = FALSE;
  floatHasExponent = FALSE;
  stringFinished   = FALSE;

  cur = s->data[s->scanPos++];

  if (cur == '_' || cur == '$' || (cur >= 'A' && cur <= 'Z') || (cur >= 'a' && cur <= 'z'))
    s->nextState.token = TK_Identifier;
  else if (cur >= '0' && cur <= '9')
  {
    if (cur == '0')
      integerBase = 8;
    s->nextState.token = TK_IntConst;
  }
  else if (cur == '.')
  {
    floatHasDecimal = TRUE;
    s->nextState.token = TK_FloatConst;
  }
  else if (cur == '"')
  {
    end = ++start; /* skip the opening quote */
    s->nextState.token = TK_StringConst;
  }
  else
  {
    end = (int)s->scanPos;
    s->nextState.token = cur;

    if (s->scanPos < s->length)
    {
      char next = s->data[s->scanPos];
      if (cur == '&' && next == '&')      s->nextState.token = TK_AndAnd;
      else if (cur == '|' && next == '|') s->nextState.token = TK_OrOr;
      else if (cur == '<' && next == '<') s->nextState.token = TK_ShiftLeft;
      else if (cur == '>' && next == '>') s->nextState.token = TK_ShiftRight;
      else if (next == '=')
      {
        switch (cur)
        {
          case '=': s->nextState.token = TK_EqEq;   break;
          case '!': s->nextState.token = TK_NotEq;  break;
          case '>': s->nextState.token = TK_GtrEq;  break;
          case '<': s->nextState.token = TK_LessEq; break;
          default:  break;
        }
      }

      if (s->nextState.token != cur)
      {
        s->scanPos++;
        end = (int)s->scanPos;
      }
    }
  }

  if (start == end)
  {
    while (s->scanPos < s->length)
    {
      cur = s->data[s->scanPos];
      switch (s->nextState.token)
      {
        default:
          break;
        case TK_Identifier:
          if (cur != '_' && (cur < 'A' || cur > 'Z') && (cur < 'a' || cur > 'z') &&
              (cur < '0' || cur > '9') && cur != '/' && cur != '\\')
            end = (int)s->scanPos;
          break;
        case TK_IntConst:
          if (cur == '.' || ((int)s->scanPos - 1 != start && cur == 'e'))
            s->nextState.token = TK_FloatConst;
          else if ((cur == 'x' || cur == 'X') && (int)s->scanPos - 1 == start)
          {
            integerBase = 16;
            break;
          }
          else
          {
            switch (integerBase)
            {
              default:
                if (cur < '0' || cur > '9')
                  end = (int)s->scanPos;
                break;
              case 8:
                if (cur < '0' || cur > '7')
                  end = (int)s->scanPos;
                break;
              case 16:
                if ((cur < '0' || cur > '9') && (cur < 'A' || cur > 'F') &&
                    (cur < 'a' || cur > 'f'))
                  end = (int)s->scanPos;
                break;
            }
            break;
          }
        case TK_FloatConst:
          if (cur < '0' || cur > '9')
          {
            if (!floatHasDecimal && cur == '.')
            {
              floatHasDecimal = TRUE;
              break;
            }
            else if (!floatHasExponent && cur == 'e')
            {
              floatHasDecimal = TRUE;
              floatHasExponent = TRUE;
              if (s->scanPos + 1 < s->length)
              {
                char next = s->data[s->scanPos + 1];
                if ((next < '0' || next > '9') && next != '+' && next != '-')
                  end = (int)s->scanPos;
                else
                  s->scanPos++;
              }
              break;
            }
            end = (int)s->scanPos;
          }
          break;
        case TK_StringConst:
          if (cur == '"')
          {
            stringFinished = TRUE;
            end = (int)s->scanPos;
            s->scanPos++;
          }
          else if (cur == '\\')
            s->scanPos++; /* loop adds the other one */
          break;
      }
      if (start == end && !stringFinished)
        s->scanPos++;
      else
        break;
    }

    if (start == end && s->scanPos == s->length)
      end = (int)s->scanPos;
  }

  if (end - start > 0 || stringFinished)
  {
    scanner_set_string(&s->nextState.string, s->data + start, end - start);
    if (s->nextState.token == TK_FloatConst)
    {
      s->nextState.decimal = atof(s->nextState.string);
      s->nextState.number  = (int)s->nextState.decimal;
      s->nextState.boolean = (s->nextState.number != 0);
    }
    else if (s->nextState.token == TK_IntConst)
    {
      s->nextState.number  = (int)strtol(s->nextState.string, NULL, integerBase);
      s->nextState.decimal = s->nextState.number;
      s->nextState.boolean = (s->nextState.number != 0);
    }
    else if (s->nextState.token == TK_Identifier)
    {
      char *p = s->nextState.string;
      while (*p)
      {
        *p = (char)tolower((unsigned char)*p);
        p++;
      }
      if (strcmp(s->nextState.string, "true") == 0)
      {
        s->nextState.token = TK_BoolConst;
        s->nextState.boolean = TRUE;
      }
      else if (strcmp(s->nextState.string, "false") == 0)
      {
        s->nextState.token = TK_BoolConst;
        s->nextState.boolean = FALSE;
      }
    }
    else if (s->nextState.token == TK_StringConst)
    {
      scanner_unescape(s->nextState.string);
    }
    if (expandState)
      scanner_expand_state(s);
    return TRUE;
  }
  s->nextState.token = TK_NoToken;
  if (expandState)
    scanner_expand_state(s);
  return FALSE;
}

void scanner_skip_line(scanner_t *s)
{
  unsigned int startLine = s->tokenLine;
  while (s->tokenLine == startLine && scanner_get_next_token(s, TRUE))
    ;
}

int scanner_get_line(const scanner_t *s)     { return (int)s->tokenLine; }
int scanner_get_line_pos(const scanner_t *s) { return (int)s->tokenLinePosition; }
void scanner_unget(scanner_t *s)             { s->needNext = TRUE; }

static void scanner_error_tok(scanner_t *s, int token)
{
  if (token < TK_NumSpecialTokens && s->token >= TK_Identifier && s->token < TK_NumSpecialTokens)
    scanner_error_cb("%d:%d:Expected '%s' but got '%s' instead.", scanner_get_line(s),
                     scanner_get_line_pos(s), TokenNames[token],
                     TokenNames[(unsigned char)s->token]);
  else if (token < TK_NumSpecialTokens && s->token >= TK_NumSpecialTokens)
    scanner_error_cb("%d:%d:Expected '%s' but got '%c' instead.", scanner_get_line(s),
                     scanner_get_line_pos(s), TokenNames[token], s->token);
  else if (token < TK_NumSpecialTokens && s->token == TK_NoToken)
    scanner_error_cb("%d:%d:Expected '%s'", scanner_get_line(s),
                     scanner_get_line_pos(s), TokenNames[token]);
  else if (token >= TK_NumSpecialTokens && s->token >= TK_Identifier && s->token < TK_NumSpecialTokens)
    scanner_error_cb("%d:%d:Expected '%c' but got '%s' instead.", scanner_get_line(s),
                     scanner_get_line_pos(s), token, TokenNames[(unsigned char)s->token]);
  else
    scanner_error_cb("%d:%d:Expected '%c' but got '%c' instead.", scanner_get_line(s),
                     scanner_get_line_pos(s), token, s->token);
}

static void scanner_error_str(scanner_t *s, const char *mustget)
{
  if (s->token < TK_NumSpecialTokens)
    scanner_error_cb("%d:%d:Expected '%s' but got '%s' instead.", scanner_get_line(s),
                     scanner_get_line_pos(s), mustget,
                     TokenNames[(unsigned char)s->token]);
  else
    scanner_error_cb("%d:%d:Expected '%s' but got '%c' instead.", scanner_get_line(s),
                     scanner_get_line_pos(s), mustget, s->token);
}

static void scanner_errorf(scanner_t *s, const char *msg, ...)
{
  char buffer[1024];
  va_list ap;
  va_start(ap, msg);
  /* C89/MSVC: no vsnprintf.  Internal callers pass short literal formats
   * (the only variadic use is "Expected String Constant or Identifier"),
   * so a fixed 1024 buffer with vsprintf cannot overflow here. */
  vsprintf(buffer, msg, ap);
  va_end(ap);
  scanner_error_cb("%d:%d:%s.", scanner_get_line(s), scanner_get_line_pos(s), buffer);
}

void scanner_must_get_token(scanner_t *s, char token)
{
  if (!scanner_check_token(s, token))
  {
    scanner_expand_state(s);
    scanner_error_tok(s, token);
  }
}

void scanner_must_get_identifier(scanner_t *s, const char *ident)
{
  if (!scanner_check_token(s, TK_Identifier) || strcmpnocase(s->string, ident))
  {
    scanner_error_str(s, ident);
    return;
  }
}

static dbool scanner_scan_integer(scanner_t *s)
{
  dbool neg = FALSE;
  if (!scanner_get_next_token(s, TRUE))
    return FALSE;
  if (s->token == '-')
  {
    if (!scanner_get_next_token(s, TRUE))
      return FALSE;
    neg = TRUE;
  }
  else if (s->token == '+')
  {
    if (!scanner_get_next_token(s, TRUE))
      return FALSE;
  }
  if (s->token != TK_IntConst)
    return FALSE;
  if (neg)
  {
    s->number  = -s->number;
    s->decimal = -s->decimal;
  }
  return TRUE;
}

static dbool scanner_scan_float(scanner_t *s)
{
  dbool neg = FALSE;
  if (!scanner_get_next_token(s, TRUE))
    return FALSE;
  if (s->token == '-')
  {
    if (!scanner_get_next_token(s, TRUE))
      return FALSE;
    neg = TRUE;
  }
  else if (s->token == '+')
  {
    if (!scanner_get_next_token(s, TRUE))
      return FALSE;
  }
  if (s->token != TK_IntConst && s->token != TK_FloatConst)
    return FALSE;
  if (neg)
  {
    s->number  = -s->number;
    s->decimal = -s->decimal;
  }
  return TRUE;
}

dbool scanner_check_integer(scanner_t *s)
{
  scanner_t saved;
  dbool res;
  memset(&saved, 0, sizeof(saved));
  scanner_save_state(s, &saved);
  res = scanner_scan_integer(s);
  if (!res)
    scanner_restore_state(s, &saved);
  scanner_free_saved(&saved);
  return res;
}

dbool scanner_check_float(scanner_t *s)
{
  scanner_t saved;
  dbool res;
  memset(&saved, 0, sizeof(saved));
  scanner_save_state(s, &saved);
  res = scanner_scan_float(s);
  if (!res)
    scanner_restore_state(s, &saved);
  scanner_free_saved(&saved);
  return res;
}

dbool scanner_check_string(scanner_t *s)
{
  return scanner_check_token(s, TK_StringConst) || scanner_check_token(s, TK_Identifier);
}

dbool scanner_string_match(scanner_t *s, const char *target)
{
  return !strcmpnocase(s->string, target);
}

void scanner_must_get_integer(scanner_t *s)
{
  if (!scanner_scan_integer(s))
    scanner_error_tok(s, TK_IntConst);
}

void scanner_must_get_float(scanner_t *s)
{
  if (!scanner_scan_float(s))
    scanner_error_tok(s, TK_FloatConst);
}

void scanner_must_get_string(scanner_t *s)
{
  if (!scanner_check_string(s))
  {
    scanner_errorf(s, "Expected String Constant or Identifier");
    return;
  }
}

dbool scanner_tokens_left(scanner_t *s)
{
  return s->scanPos < s->length;
}

/* ZDoom-derived string unescaper. */
static void scanner_unescape(char *str)
{
  char *p = str, c;
  int i;

  while ((c = *p++) != 0)
  {
    if (c != '\\')
    {
      *str++ = c;
    }
    else if (*p)
    {
      switch (*p)
      {
        case 'a': *str++ = '\a'; break;
        case 'b': *str++ = '\b'; break;
        case 'f': *str++ = '\f'; break;
        case 'n': *str++ = '\n'; break;
        case 't': *str++ = '\t'; break;
        case 'r': *str++ = '\r'; break;
        case 'v': *str++ = '\v'; break;
        case '?': *str++ = '\?'; break;
        case '\n': break;
        case 'x':
        case 'X':
          c = 0;
          for (i = 0; i < 2; i++)
          {
            p++;
            if (*p >= '0' && *p <= '9')      c = (char)((c << 4) + *p - '0');
            else if (*p >= 'a' && *p <= 'f') c = (char)((c << 4) + 10 + *p - 'a');
            else if (*p >= 'A' && *p <= 'F') c = (char)((c << 4) + 10 + *p - 'A');
            else { p--; break; }
          }
          *str++ = c;
          break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7':
          c = (char)(*p - '0');
          for (i = 0; i < 2; i++)
          {
            p++;
            if (*p >= '0' && *p <= '7') c = (char)((c << 3) + *p - '0');
            else { p--; break; }
          }
          *str++ = c;
          break;
        default:
          *str++ = *p;
          break;
      }
      p++;
    }
    else
    {
      /* trailing backslash */
    }
  }
  *str = 0;
}
