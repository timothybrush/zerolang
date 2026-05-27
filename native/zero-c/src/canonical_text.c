#include "canonical_text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const ZCanonicalTokenVec *tokens;
  ZCanonicalTree *tree;
  ZCanonicalFacts *facts;
  ZDiag *diag;
  size_t pos;
} CanonParser;

static bool canon_text_eq(const char *left, const char *right) {
  if (!left || !right) return left == right;
  while (*left && *right && *left == *right) {
    left++;
    right++;
  }
  return *left == 0 && *right == 0;
}

static bool canon_is_word_text(const ZCanonicalToken *token, const char *text) {
  return token && token->kind == Z_CANON_TOKEN_WORD && canon_text_eq(token->text, text);
}

static bool canon_is_symbol_text(const ZCanonicalToken *token, const char *text) {
  return token && token->kind == Z_CANON_TOKEN_SYMBOL && canon_text_eq(token->text, text);
}

static bool canon_fail(ZDiag *diag, const ZCanonicalToken *token, const char *message, const char *expected, const char *actual) {
  if (diag) {
    diag->code = 100;
    snprintf(diag->message, sizeof(diag->message), "%s", message);
    snprintf(diag->expected, sizeof(diag->expected), "%s", expected ? expected : "");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "");
    snprintf(diag->help, sizeof(diag->help), "use canonical .0 text source");
    diag->line = token ? token->line : 1;
    diag->column = token ? token->column : 1;
    diag->length = token && token->length ? (int)token->length : 1;
  }
  return false;
}

static void canon_push_token(ZCanonicalTokenVec *vec, ZCanonicalToken token) {
  if (vec->len == vec->cap) {
    vec->cap = z_grow_capacity(vec->cap, vec->len + 1, 64);
    vec->items = z_checked_reallocarray(vec->items, vec->cap, sizeof(ZCanonicalToken));
  }
  vec->items[vec->len++] = token;
}

static void canon_push_node(ZCanonicalTree *tree, ZCanonicalNode node) {
  if (tree->len == tree->cap) {
    tree->cap = z_grow_capacity(tree->cap, tree->len + 1, 64);
    tree->items = z_checked_reallocarray(tree->items, tree->cap, sizeof(ZCanonicalNode));
  }
  tree->items[tree->len++] = node;
}

static char *canon_slice(const char *source, size_t start, size_t end) {
  return z_strndup(source + start, end - start);
}

static bool canon_word_start(char ch) {
  return isalpha((unsigned char)ch) || ch == '_';
}

static bool canon_word_part(char ch) {
  return isalnum((unsigned char)ch) || ch == '_';
}

static int canon_hex_digit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

static bool canon_scan_string(const char *source, size_t *offset, int *column, ZCanonicalTokenVec *tokens, ZDiag *diag, int line) {
  size_t start = *offset;
  int start_col = *column;
  (*offset)++;
  (*column)++;
  while (source[*offset] && source[*offset] != '"') {
    if (source[*offset] == '\n') return canon_fail(diag, NULL, "unterminated string literal", "closing quote", "newline");
    if (source[*offset] == '\\' && source[*offset + 1]) {
      *offset += 2;
      *column += 2;
      continue;
    }
    (*offset)++;
    (*column)++;
  }
  if (source[*offset] != '"') return canon_fail(diag, NULL, "unterminated string literal", "closing quote", "end of file");
  (*offset)++;
  (*column)++;
  canon_push_token(tokens, (ZCanonicalToken){Z_CANON_TOKEN_STRING, canon_slice(source, start, *offset), line, start_col, start, *offset - start});
  return true;
}

static bool canon_scan_char(const char *source, size_t *offset, int *column, ZCanonicalTokenVec *tokens, ZDiag *diag, int line) {
  size_t start = *offset;
  int start_col = *column;
  ZCanonicalToken at_start = {Z_CANON_TOKEN_CHAR, NULL, line, start_col, start, 1};
  (*offset)++;
  (*column)++;

  if (!source[*offset] || source[*offset] == '\n' || source[*offset] == '\r' || source[*offset] == '\'') {
    return canon_fail(diag, &at_start, "malformed character literal", "one byte character literal", "empty");
  }

  if (source[*offset] == '\\') {
    (*offset)++;
    (*column)++;
    char escaped = source[*offset];
    if (!escaped || escaped == '\n' || escaped == '\r') return canon_fail(diag, &at_start, "malformed character escape", "one byte character literal", "newline");
    if (escaped == 'x') {
      char high_ch = source[*offset + 1];
      char low_ch = high_ch ? source[*offset + 2] : 0;
      if (canon_hex_digit(high_ch) < 0 || canon_hex_digit(low_ch) < 0) {
        return canon_fail(diag, &at_start, "malformed hex character escape", "two hex digits", "invalid escape");
      }
      *offset += 3;
      *column += 3;
    } else {
      if (escaped != 'n' && escaped != 'r' && escaped != 't' && escaped != '0' && escaped != '\'' && escaped != '"' && escaped != '\\') {
        return canon_fail(diag, &at_start, "invalid character escape", "one byte character literal", "invalid escape");
      }
      (*offset)++;
      (*column)++;
    }
  } else {
    if ((unsigned char)source[*offset] >= 128) return canon_fail(diag, &at_start, "character literal must be one byte", "ASCII byte", "non-ASCII");
    (*offset)++;
    (*column)++;
  }

  if (source[*offset] != '\'') return canon_fail(diag, &at_start, "character literal must contain exactly one byte", "closing quote", "extra content");
  (*offset)++;
  (*column)++;
  canon_push_token(tokens, (ZCanonicalToken){Z_CANON_TOKEN_CHAR, canon_slice(source, start, *offset), line, start_col, start, *offset - start});
  return true;
}

static bool canon_two_char_symbol(const char *source, size_t offset) {
  const char *pairs[] = {"->", "==", "!=", "<=", ">=", "&&", "||", "..", "=>"};
  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
    if (source[offset] == pairs[i][0] && source[offset + 1] == pairs[i][1]) return true;
  }
  return false;
}

ZCanonicalTokenVec z_canonical_text_tokenize(const char *source, ZDiag *diag) {
  ZCanonicalTokenVec tokens = {0};
  size_t offset = 0;
  int line = 1;
  int column = 1;
  while (source && source[offset]) {
    char ch = source[offset];
    if (ch == '\t') {
      canon_fail(diag, NULL, "tabs are not canonical text whitespace", "spaces", "tab");
      break;
    }
    if (ch == ' ' || ch == '\r') {
      offset++;
      column++;
      continue;
    }
    if (ch == '\n') {
      canon_push_token(&tokens, (ZCanonicalToken){Z_CANON_TOKEN_NEWLINE, z_strdup("\n"), line, column, offset, 1});
      offset++;
      line++;
      column = 1;
      continue;
    }
    if (ch == '/' && source[offset + 1] == '/') {
      size_t start = offset;
      int start_col = column;
      while (source[offset] && source[offset] != '\n') {
        offset++;
        column++;
      }
      canon_push_token(&tokens, (ZCanonicalToken){Z_CANON_TOKEN_COMMENT, canon_slice(source, start, offset), line, start_col, start, offset - start});
      continue;
    }
    if (ch == '"') {
      if (!canon_scan_string(source, &offset, &column, &tokens, diag, line)) break;
      continue;
    }
    if (ch == '\'') {
      if (!canon_scan_char(source, &offset, &column, &tokens, diag, line)) break;
      continue;
    }
    if (canon_word_start(ch)) {
      size_t start = offset;
      int start_col = column;
      while (canon_word_part(source[offset])) {
        offset++;
        column++;
      }
      canon_push_token(&tokens, (ZCanonicalToken){Z_CANON_TOKEN_WORD, canon_slice(source, start, offset), line, start_col, start, offset - start});
      continue;
    }
    if (isdigit((unsigned char)ch)) {
      size_t start = offset;
      int start_col = column;
      while (source[offset] && (isalnum((unsigned char)source[offset]) || source[offset] == '_' || source[offset] == '.')) {
        if (source[offset] == '.' && source[offset + 1] == '.') break;
        offset++;
        column++;
      }
      canon_push_token(&tokens, (ZCanonicalToken){Z_CANON_TOKEN_NUMBER, canon_slice(source, start, offset), line, start_col, start, offset - start});
      continue;
    }
    size_t len = canon_two_char_symbol(source, offset) ? 2 : 1;
    canon_push_token(&tokens, (ZCanonicalToken){Z_CANON_TOKEN_SYMBOL, canon_slice(source, offset, offset + len), line, column, offset, len});
    offset += len;
    column += (int)len;
  }
  canon_push_token(&tokens, (ZCanonicalToken){Z_CANON_TOKEN_EOF, z_strdup(""), line, column, offset, 0});
  return tokens;
}

static const ZCanonicalToken *canon_peek(CanonParser *parser) {
  return parser->pos < parser->tokens->len ? &parser->tokens->items[parser->pos] : NULL;
}

static void canon_skip_newlines(CanonParser *parser) {
  while (canon_peek(parser) && (canon_peek(parser)->kind == Z_CANON_TOKEN_NEWLINE || canon_peek(parser)->kind == Z_CANON_TOKEN_COMMENT)) {
    if (canon_peek(parser)->kind == Z_CANON_TOKEN_COMMENT && parser->facts) parser->facts->comment_count++;
    parser->pos++;
  }
}

static bool canon_accept_symbol(CanonParser *parser, const char *text) {
  if (!canon_is_symbol_text(canon_peek(parser), text)) return false;
  parser->pos++;
  return true;
}

static bool canon_accept_word(CanonParser *parser, const char *text) {
  if (!canon_is_word_text(canon_peek(parser), text)) return false;
  parser->pos++;
  return true;
}

static bool canon_expect_symbol(CanonParser *parser, const char *text, const char *message) {
  if (canon_accept_symbol(parser, text)) return true;
  return canon_fail(parser->diag, canon_peek(parser), message, text, canon_peek(parser) ? canon_peek(parser)->text : "end of file");
}

static bool canon_expect_word_token(CanonParser *parser, const char *message) {
  const ZCanonicalToken *token = canon_peek(parser);
  if (token && token->kind == Z_CANON_TOKEN_WORD) {
    parser->pos++;
    return true;
  }
  return canon_fail(parser->diag, token, message, "identifier", token ? token->text : "end of file");
}

static bool canon_parse_until(CanonParser *parser, const char *stop_a, const char *stop_b, bool allow_empty, bool balance_angles) {
  size_t start = parser->pos;
  int paren = 0, bracket = 0, brace = 0, angle = 0;
  while (canon_peek(parser) && canon_peek(parser)->kind != Z_CANON_TOKEN_EOF) {
    const ZCanonicalToken *token = canon_peek(parser);
    if (paren == 0 && bracket == 0 && brace == 0 && angle == 0) {
      if (token->kind == Z_CANON_TOKEN_COMMENT) break;
      if ((stop_a && canon_is_symbol_text(token, stop_a)) || (stop_b && canon_is_symbol_text(token, stop_b))) break;
      if ((stop_a && canon_is_word_text(token, stop_a)) || (stop_b && canon_is_word_text(token, stop_b))) break;
      if ((stop_a || stop_b) && canon_is_symbol_text(token, "!")) break;
      if (token->kind == Z_CANON_TOKEN_NEWLINE) break;
    }
    if (balance_angles && canon_is_symbol_text(token, "<")) angle++;
    else if (balance_angles && canon_is_symbol_text(token, ">")) {
      if (angle == 0) break;
      angle--;
    } else if (canon_is_symbol_text(token, "(")) paren++;
    else if (canon_is_symbol_text(token, ")")) {
      if (paren == 0) break;
      paren--;
    } else if (canon_is_symbol_text(token, "[")) bracket++;
    else if (canon_is_symbol_text(token, "]")) {
      if (bracket == 0) break;
      bracket--;
    } else if (canon_is_symbol_text(token, "{")) brace++;
    else if (canon_is_symbol_text(token, "}")) {
      if (brace == 0) break;
      brace--;
    }
    parser->pos++;
  }
  if (!allow_empty && parser->pos == start) return canon_fail(parser->diag, canon_peek(parser), "expected expression", "expression", "empty");
  if (paren || bracket || brace || angle) return canon_fail(parser->diag, canon_peek(parser), "unbalanced expression delimiters", "balanced delimiters", "unbalanced");
  return true;
}

static bool canon_expr_operand_token(const ZCanonicalToken *token) {
  return token && (token->kind == Z_CANON_TOKEN_WORD || token->kind == Z_CANON_TOKEN_STRING || token->kind == Z_CANON_TOKEN_CHAR || token->kind == Z_CANON_TOKEN_NUMBER);
}

static bool canon_expr_keyword_operator(const ZCanonicalToken *token) {
  return token && token->kind == Z_CANON_TOKEN_WORD && (canon_text_eq(token->text, "as") || canon_text_eq(token->text, "rescue") || canon_text_eq(token->text, "err") || canon_text_eq(token->text, "in"));
}

static bool canon_expr_open_symbol(const ZCanonicalToken *token) {
  return canon_is_symbol_text(token, "(") || canon_is_symbol_text(token, "[") || canon_is_symbol_text(token, "{");
}

static bool canon_expr_close_symbol(const ZCanonicalToken *token) {
  return canon_is_symbol_text(token, ")") || canon_is_symbol_text(token, "]") || canon_is_symbol_text(token, "}");
}

static bool canon_expr_binary_symbol(const ZCanonicalToken *token) {
  return canon_is_symbol_text(token, "+") || canon_is_symbol_text(token, "-") || canon_is_symbol_text(token, "*") || canon_is_symbol_text(token, "/") ||
    canon_is_symbol_text(token, "%") || canon_is_symbol_text(token, "==") || canon_is_symbol_text(token, "!=") || canon_is_symbol_text(token, "<") ||
    canon_is_symbol_text(token, "<=") || canon_is_symbol_text(token, ">") || canon_is_symbol_text(token, ">=") || canon_is_symbol_text(token, "&&") ||
    canon_is_symbol_text(token, "||") || canon_is_symbol_text(token, "=>");
}

static bool canon_expr_prefix_symbol(const ZCanonicalToken *token) {
  return canon_is_symbol_text(token, "&") || canon_is_symbol_text(token, "*") || canon_is_symbol_text(token, "!") || canon_is_symbol_text(token, "-") || canon_is_symbol_text(token, "+");
}

static bool canon_validate_expr_shape(CanonParser *parser, size_t start, size_t end) {
  if (start == end) return true;
  bool expect_operand = true;
  bool saw_operand = false;
  int delimiter_depth = 0;
  int brace_depth = 0;
  for (size_t i = start; i < end; i++) {
    const ZCanonicalToken *token = &parser->tokens->items[i];
    const ZCanonicalToken *previous = i > start ? &parser->tokens->items[i - 1] : NULL;

    if (canon_expr_keyword_operator(token)) {
      if (canon_text_eq(token->text, "err") || !expect_operand) {
        expect_operand = true;
        continue;
      }
      return canon_fail(parser->diag, token, "expected expression before keyword", "expression", token->text);
    }

    if (token->kind != Z_CANON_TOKEN_SYMBOL) {
      if (!canon_expr_operand_token(token)) return canon_fail(parser->diag, token, "unexpected token in expression", "expression", token->text);
      if (canon_text_eq(token->text, "mut") && previous && canon_is_symbol_text(previous, "&") && expect_operand) continue;
      if (!expect_operand) return canon_fail(parser->diag, token, "expected operator between expressions", "operator", token->text);
      expect_operand = false;
      saw_operand = true;
      continue;
    }

    if (canon_expr_open_symbol(token)) {
      if (canon_is_symbol_text(token, "{") && expect_operand) return canon_fail(parser->diag, token, "expected expression before object literal", "expression", "{");
      delimiter_depth++;
      if (canon_is_symbol_text(token, "{")) brace_depth++;
      expect_operand = true;
      continue;
    }
    if (canon_expr_close_symbol(token)) {
      if (expect_operand && !(previous && (canon_expr_open_symbol(previous) || canon_is_symbol_text(previous, "..")))) {
        return canon_fail(parser->diag, token, "expected expression before delimiter", "expression", token->text);
      }
      if (delimiter_depth > 0) delimiter_depth--;
      if (canon_is_symbol_text(token, "}") && brace_depth > 0) brace_depth--;
      expect_operand = false;
      saw_operand = true;
      continue;
    }
    if (canon_is_symbol_text(token, ",")) {
      if (expect_operand) return canon_fail(parser->diag, token, "expected expression before comma", "expression", ",");
      if (delimiter_depth == 0) return canon_fail(parser->diag, token, "unexpected comma in expression", "expression delimiter", ",");
      expect_operand = true;
      continue;
    }
    if (canon_is_symbol_text(token, ":")) {
      if (expect_operand) return canon_fail(parser->diag, token, "expected field name before ':'", "field name", ":");
      if (brace_depth == 0) return canon_fail(parser->diag, token, "unexpected ':' in expression", "object field", ":");
      expect_operand = true;
      continue;
    }
    if (canon_is_symbol_text(token, ".")) {
      if (expect_operand) return canon_fail(parser->diag, token, "expected expression before member access", "expression", ".");
      expect_operand = true;
      continue;
    }
    if (canon_is_symbol_text(token, "..")) {
      expect_operand = true;
      continue;
    }
    if (expect_operand && canon_expr_prefix_symbol(token)) continue;
    if (canon_expr_binary_symbol(token)) {
      if (expect_operand) return canon_fail(parser->diag, token, "expected expression before operator", "expression", token->text);
      expect_operand = true;
      continue;
    }
    return canon_fail(parser->diag, token, "unexpected token in expression", "expression", token->text);
  }
  if (!saw_operand) return canon_fail(parser->diag, end > start ? &parser->tokens->items[end - 1] : canon_peek(parser), "expected expression", "expression", "empty");
  if (expect_operand) return canon_fail(parser->diag, &parser->tokens->items[end - 1], "incomplete expression", "expression", parser->tokens->items[end - 1].text);
  return true;
}

static bool canon_validate_expr(CanonParser *parser, size_t start, size_t end) {
  if (!canon_validate_expr_shape(parser, start, end)) return false;
  size_t compare_counts[128] = {0};
  size_t compare_depth = 0;
  for (size_t i = start; i < end; i++) {
    const ZCanonicalToken *token = &parser->tokens->items[i];
    if (canon_is_symbol_text(token, "(") || canon_is_symbol_text(token, "[") || canon_is_symbol_text(token, "{")) {
      if (compare_depth + 1 < sizeof(compare_counts) / sizeof(compare_counts[0])) compare_depth++;
      compare_counts[compare_depth] = 0;
      continue;
    }
    if (canon_is_symbol_text(token, ")") || canon_is_symbol_text(token, "]") || canon_is_symbol_text(token, "}")) {
      compare_counts[compare_depth] = 0;
      if (compare_depth > 0) compare_depth--;
      continue;
    }
    if (canon_is_symbol_text(token, "&&") || canon_is_symbol_text(token, "||") || canon_is_symbol_text(token, ",")) {
      compare_counts[compare_depth] = 0;
      continue;
    }
    if (canon_is_symbol_text(token, "<")) {
      int depth = 1;
      bool generic_call = false;
      for (size_t j = i + 1; j < end; j++) {
        if (canon_is_symbol_text(&parser->tokens->items[j], "<")) depth++;
        else if (canon_is_symbol_text(&parser->tokens->items[j], ">") && --depth == 0) {
          generic_call = j + 1 < end && (canon_is_symbol_text(&parser->tokens->items[j + 1], "(") || canon_is_symbol_text(&parser->tokens->items[j + 1], "{"));
          if (generic_call) i = j;
          break;
        }
      }
      if (!generic_call && ++compare_counts[compare_depth] > 1) {
        return canon_fail(parser->diag, token, "chained comparisons are not canonical text", "separate comparisons", "chained comparison");
      }
      continue;
    }
    if ((canon_is_symbol_text(token, "==") || canon_is_symbol_text(token, "!=") || canon_is_symbol_text(token, "<=") || canon_is_symbol_text(token, ">") || canon_is_symbol_text(token, ">=")) && ++compare_counts[compare_depth] > 1) {
      return canon_fail(parser->diag, token, "chained comparisons are not canonical text", "separate comparisons", "chained comparison");
    }
    if (i + 1 < end && token->kind == Z_CANON_TOKEN_WORD) {
      const ZCanonicalToken *previous = i > start ? &parser->tokens->items[i - 1] : NULL;
      const ZCanonicalToken *next = &parser->tokens->items[i + 1];
      if (canon_text_eq(token->text, "as") || canon_text_eq(token->text, "rescue") || canon_text_eq(token->text, "err") || canon_text_eq(token->text, "in")) continue;
      if (next->kind == Z_CANON_TOKEN_WORD && (canon_text_eq(next->text, "as") || canon_text_eq(next->text, "rescue") || canon_text_eq(next->text, "err") || canon_text_eq(next->text, "in"))) continue;
      if (canon_text_eq(token->text, "mut") && previous && canon_is_symbol_text(previous, "&")) continue;
      if (next->kind == Z_CANON_TOKEN_STRING || next->kind == Z_CANON_TOKEN_NUMBER || next->kind == Z_CANON_TOKEN_WORD) {
        return canon_fail(parser->diag, next, "space calls are not canonical text", "call parentheses", next->text);
      }
    }
  }
  return true;
}

static bool canon_parse_expr_line(CanonParser *parser, bool allow_empty) {
  size_t start = parser->pos;
  if (!canon_parse_until(parser, NULL, NULL, allow_empty, false)) return false;
  return canon_validate_expr(parser, start, parser->pos);
}

static bool canon_type_atom(const ZCanonicalToken *token) {
  return token && (token->kind == Z_CANON_TOKEN_WORD || token->kind == Z_CANON_TOKEN_NUMBER || token->kind == Z_CANON_TOKEN_STRING || token->kind == Z_CANON_TOKEN_CHAR);
}

static bool canon_validate_type_span(CanonParser *parser, size_t start, size_t end) {
  bool bracket_prefix[64] = {0};
  size_t bracket_depth = 0;
  bool expect_atom = true;
  for (size_t i = start; i < end; i++) {
    const ZCanonicalToken *token = &parser->tokens->items[i];
    if (canon_type_atom(token)) {
      if (!expect_atom) return canon_fail(parser->diag, token, "missing separator in type", "comma or delimiter", token->text);
      expect_atom = false;
      continue;
    }
    if (canon_is_symbol_text(token, "[")) {
      if (bracket_depth < sizeof(bracket_prefix) / sizeof(bracket_prefix[0])) bracket_prefix[bracket_depth++] = expect_atom;
      expect_atom = true;
      continue;
    }
    if (canon_is_symbol_text(token, "<") || canon_is_symbol_text(token, "(")) {
      if (expect_atom) return canon_fail(parser->diag, token, "unexpected type delimiter", "type", token->text);
      expect_atom = true;
      continue;
    }
    if (canon_is_symbol_text(token, "]")) {
      if (expect_atom) return canon_fail(parser->diag, token, "expected type before delimiter", "type", token->text);
      bool prefix = bracket_depth > 0 && bracket_prefix[--bracket_depth];
      expect_atom = prefix;
      continue;
    }
    if (canon_is_symbol_text(token, ">") || canon_is_symbol_text(token, ")")) {
      if (expect_atom) return canon_fail(parser->diag, token, "expected type before delimiter", "type", token->text);
      expect_atom = false;
      continue;
    }
    if (canon_is_symbol_text(token, ",")) {
      if (expect_atom) return canon_fail(parser->diag, token, "expected type before comma", "type", ",");
      expect_atom = true;
      continue;
    }
    if (canon_is_symbol_text(token, ".") || canon_is_symbol_text(token, "->")) {
      if (expect_atom) return canon_fail(parser->diag, token, "expected type before separator", "type", token->text);
      expect_atom = true;
      continue;
    }
    if (canon_is_symbol_text(token, "&") || canon_is_symbol_text(token, "*") || canon_is_symbol_text(token, "?")) {
      if (!expect_atom) return canon_fail(parser->diag, token, "unexpected type prefix", "type separator", token->text);
      continue;
    }
    return canon_fail(parser->diag, token, "unexpected token in type", "type", token->text);
  }
  if (expect_atom) return canon_fail(parser->diag, end > start ? &parser->tokens->items[end - 1] : canon_peek(parser), "incomplete type", "type", "delimiter");
  return true;
}

static bool canon_parse_type_until(CanonParser *parser, const char *stop_a, const char *stop_b) {
  size_t start = parser->pos;
  if (!canon_parse_until(parser, stop_a, stop_b, false, true)) return false;
  return canon_validate_type_span(parser, start, parser->pos);
}

static bool canon_parse_type_params(CanonParser *parser) {
  if (!canon_accept_symbol(parser, "<")) return true;
  if (canon_accept_symbol(parser, ">")) return canon_fail(parser->diag, canon_peek(parser), "expected type parameter", "type parameter", ">");
  while (canon_peek(parser) && canon_peek(parser)->kind != Z_CANON_TOKEN_EOF) {
    if (canon_accept_word(parser, "static")) {
      if (!canon_expect_word_token(parser, "expected static type parameter name")) return false;
      if (!canon_expect_symbol(parser, ":", "expected ':' after static type parameter")) return false;
      if (!canon_parse_type_until(parser, ",", ">")) return false;
    } else {
      if (!canon_expect_word_token(parser, "expected type parameter name")) return false;
      if (canon_accept_symbol(parser, ":") && !canon_parse_type_until(parser, ",", ">")) return false;
    }
    if (canon_accept_symbol(parser, ">")) return true;
    if (!canon_expect_symbol(parser, ",", "expected comma between type parameters")) return false;
    if (canon_is_symbol_text(canon_peek(parser), ">")) return canon_fail(parser->diag, canon_peek(parser), "expected type parameter after comma", "type parameter", ">");
  }
  return canon_fail(parser->diag, canon_peek(parser), "unterminated type parameter list", ">", "end of file");
}

static bool canon_parse_params(CanonParser *parser) {
  if (!canon_expect_symbol(parser, "(", "expected parameter list")) return false;
  if (canon_accept_symbol(parser, ")")) return true;
  while (true) {
    if (!canon_expect_word_token(parser, "expected parameter name")) return false;
    if (!canon_expect_symbol(parser, ":", "expected ':' after parameter name")) return false;
    if (!canon_parse_type_until(parser, ",", ")")) return false;
    if (canon_accept_symbol(parser, ")")) return true;
    if (!canon_expect_symbol(parser, ",", "expected comma between parameters")) return false;
  }
}

static bool canon_parse_raises(CanonParser *parser) {
  if (!canon_accept_word(parser, "raises")) return true;
  if (!canon_accept_symbol(parser, "[")) {
    if (canon_is_symbol_text(canon_peek(parser), "{")) {
      const ZCanonicalToken *next = parser->pos + 1 < parser->tokens->len ? &parser->tokens->items[parser->pos + 1] : NULL;
      const ZCanonicalToken *after = parser->pos + 2 < parser->tokens->len ? &parser->tokens->items[parser->pos + 2] : NULL;
      if (next && next->kind == Z_CANON_TOKEN_WORD && after && canon_is_symbol_text(after, "}")) {
        return canon_fail(parser->diag, canon_peek(parser), "named error sets use brackets", "raises [Error]", "raises { Error }");
      }
    }
    return true;
  }
  if (!canon_expect_word_token(parser, "expected named error")) return false;
  while (canon_accept_symbol(parser, ",")) {
    if (!canon_expect_word_token(parser, "expected named error after comma")) return false;
  }
  return canon_expect_symbol(parser, "]", "expected closing named error list");
}

static bool canon_parse_block(CanonParser *parser, size_t depth);
static bool canon_parse_statement(CanonParser *parser, size_t depth);

static bool canon_expect_statement_end(CanonParser *parser, const char *message) {
  const ZCanonicalToken *token = canon_peek(parser);
  return (!token || token->kind == Z_CANON_TOKEN_EOF || token->kind == Z_CANON_TOKEN_NEWLINE || token->kind == Z_CANON_TOKEN_COMMENT || canon_is_symbol_text(token, "}")) ? true : canon_fail(parser->diag, token, message, "line end", token->text);
}

static bool canon_parse_signature(CanonParser *parser, bool body, size_t depth) {
  if (!canon_expect_word_token(parser, "expected function name")) return false;
  while (canon_accept_symbol(parser, ".")) {
    if (!canon_expect_word_token(parser, "expected method name")) return false;
  }
  if (!canon_parse_type_params(parser)) return false;
  if (!canon_parse_params(parser)) return false;
  if (!canon_expect_symbol(parser, "->", "expected return type arrow")) return false;
  if (!canon_parse_type_until(parser, "raises", "{")) return false;
  if (canon_accept_symbol(parser, "!")) return canon_fail(parser->diag, canon_peek(parser), "fallibility uses raises", "raises", "!");
  if (!canon_parse_raises(parser)) return false;
  return body ? canon_parse_block(parser, depth) : true;
}

static bool canon_parse_block(CanonParser *parser, size_t depth) {
  const ZCanonicalToken *open = canon_peek(parser);
  if (!canon_expect_symbol(parser, "{", "expected block")) return false;
  canon_push_node(parser->tree, (ZCanonicalNode){Z_CANON_NODE_BLOCK, parser->pos - 1, 1, depth, open->line, open->column});
  if (parser->facts) {
    parser->facts->block_count++;
    if (depth > parser->facts->max_block_depth) parser->facts->max_block_depth = depth;
  }
  canon_skip_newlines(parser);
  while (canon_peek(parser) && !canon_is_symbol_text(canon_peek(parser), "}") && canon_peek(parser)->kind != Z_CANON_TOKEN_EOF) {
    if (!canon_parse_statement(parser, depth + 1)) return false;
    canon_skip_newlines(parser);
  }
  return canon_expect_symbol(parser, "}", "expected closing block");
}

static bool canon_parse_let_or_var(CanonParser *parser) {
  if (!canon_expect_word_token(parser, "expected binding name")) return false;
  if (!canon_expect_symbol(parser, ":", "canonical local bindings require a type annotation")) return false;
  if (!canon_parse_type_until(parser, "=", NULL)) return false;
  if (!canon_expect_symbol(parser, "=", "expected binding initializer")) return false;
  return canon_parse_expr_line(parser, false);
}

static bool canon_parse_if(CanonParser *parser, size_t depth) {
  size_t start = parser->pos;
  if (!canon_parse_until(parser, "{", NULL, false, false)) return false;
  if (!canon_validate_expr(parser, start, parser->pos)) return false;
  if (!canon_parse_block(parser, depth)) return false;
  canon_skip_newlines(parser);
  if (canon_accept_word(parser, "else")) return canon_parse_block(parser, depth);
  return true;
}

static bool canon_parse_match(CanonParser *parser, size_t depth) {
  size_t expr_start = parser->pos;
  if (!canon_parse_until(parser, "{", NULL, false, false)) return false;
  if (!canon_validate_expr(parser, expr_start, parser->pos)) return false;
  const ZCanonicalToken *open = canon_peek(parser);
  if (!canon_expect_symbol(parser, "{", "expected match body")) return false;
  canon_push_node(parser->tree, (ZCanonicalNode){Z_CANON_NODE_BLOCK, parser->pos - 1, 1, depth, open->line, open->column});
  if (parser->facts) {
    parser->facts->block_count++;
    if (depth > parser->facts->max_block_depth) parser->facts->max_block_depth = depth;
  }
  canon_skip_newlines(parser);
  while (canon_peek(parser) && !canon_is_symbol_text(canon_peek(parser), "}") && canon_peek(parser)->kind != Z_CANON_TOKEN_EOF) {
    size_t pattern_start = parser->pos;
    if (!canon_parse_until(parser, "{", NULL, false, false)) return false;
    if (!canon_validate_expr(parser, pattern_start, parser->pos)) return false;
    if (!canon_parse_block(parser, depth + 1)) return false;
    canon_skip_newlines(parser);
  }
  return canon_expect_symbol(parser, "}", "expected closing match body");
}

static bool canon_parse_statement(CanonParser *parser, size_t depth) {
  canon_skip_newlines(parser);
  const ZCanonicalToken *start = canon_peek(parser);
  if (!start || start->kind == Z_CANON_TOKEN_EOF || canon_is_symbol_text(start, "}")) return true;
  canon_push_node(parser->tree, (ZCanonicalNode){Z_CANON_NODE_STMT, parser->pos, 0, depth, start->line, start->column});
  if (parser->facts) parser->facts->statement_count++;
  if (canon_accept_word(parser, "mut")) return canon_fail(parser->diag, start, "mutable locals use var", "var", "mut");
  if (canon_accept_word(parser, "let") || canon_accept_word(parser, "var")) return canon_parse_let_or_var(parser);
  if (canon_accept_word(parser, "return")) return canon_parse_expr_line(parser, true);
  if (canon_accept_word(parser, "check") || canon_accept_word(parser, "expect")) return canon_parse_expr_line(parser, false);
  if (canon_accept_word(parser, "raise")) return canon_expect_word_token(parser, "expected error name") && canon_expect_statement_end(parser, "unexpected tokens after raise statement");
  if (canon_accept_word(parser, "if")) return canon_parse_if(parser, depth);
  if (canon_accept_word(parser, "while")) {
    size_t expr_start = parser->pos;
    if (!canon_parse_until(parser, "{", NULL, false, false)) return false;
    if (!canon_validate_expr(parser, expr_start, parser->pos)) return false;
    return canon_parse_block(parser, depth);
  }
  if (canon_accept_word(parser, "for")) {
    if (!canon_expect_word_token(parser, "expected loop binding")) return false;
    if (!canon_accept_word(parser, "in")) return canon_fail(parser->diag, canon_peek(parser), "expected for range", "in", canon_peek(parser)->text);
    size_t expr_start = parser->pos;
    if (!canon_parse_until(parser, "{", NULL, false, false)) return false;
    if (!canon_validate_expr(parser, expr_start, parser->pos)) return false;
    return canon_parse_block(parser, depth);
  }
  if (canon_accept_word(parser, "match")) return canon_parse_match(parser, depth);
  if (canon_accept_word(parser, "break") || canon_accept_word(parser, "continue")) return canon_expect_statement_end(parser, "unexpected tokens after control statement");
  if (canon_accept_word(parser, "defer")) {
    if (canon_is_symbol_text(canon_peek(parser), "{")) return canon_parse_block(parser, depth);
    return canon_parse_expr_line(parser, false);
  }
  return canon_parse_expr_line(parser, false);
}

static bool canon_parse_field_list(CanonParser *parser, bool typed_fields) {
  if (!canon_expect_symbol(parser, "{", "expected declaration body")) return false;
  canon_skip_newlines(parser);
  if (canon_accept_symbol(parser, "}")) return true;
  while (true) {
    canon_skip_newlines(parser);
    if (!canon_expect_word_token(parser, "expected field or variant name")) return false;
    if (typed_fields) {
      if (!canon_expect_symbol(parser, ":", "expected ':' after field name")) return false;
      if (!canon_parse_type_until(parser, ",", "}")) return false;
    }
    if (!canon_expect_symbol(parser, ",", "expected trailing comma in declaration list")) return false;
    canon_skip_newlines(parser);
    if (canon_accept_symbol(parser, "}")) return true;
  }
}

static bool canon_parse_interface(CanonParser *parser) {
  if (!canon_expect_word_token(parser, "expected interface name")) return false;
  if (!canon_expect_symbol(parser, "{", "expected interface body")) return false;
  canon_skip_newlines(parser);
  while (!canon_is_symbol_text(canon_peek(parser), "}") && canon_peek(parser)->kind != Z_CANON_TOKEN_EOF) {
    if (!canon_accept_word(parser, "fn")) return canon_fail(parser->diag, canon_peek(parser), "expected interface method", "fn", canon_peek(parser)->text);
    if (!canon_parse_signature(parser, false, 1)) return false;
    canon_skip_newlines(parser);
  }
  return canon_expect_symbol(parser, "}", "expected interface close");
}

static bool canon_expect_declaration_end(CanonParser *parser, const char *message) {
  const ZCanonicalToken *token = canon_peek(parser);
  return (!token || token->kind == Z_CANON_TOKEN_EOF || token->kind == Z_CANON_TOKEN_NEWLINE || token->kind == Z_CANON_TOKEN_COMMENT) ? true : canon_fail(parser->diag, token, message, "line end", token->text);
}

static bool canon_parse_type_declaration(CanonParser *parser, bool was_extern) {
  if (!canon_expect_word_token(parser, "expected type name")) return false;
  if (!canon_parse_type_params(parser)) return false;
  if (was_extern) {
    if (canon_is_symbol_text(canon_peek(parser), "{")) return canon_parse_field_list(parser, true);
    return canon_expect_declaration_end(parser, "unexpected tokens after extern type declaration");
  }
  if (parser->facts) parser->facts->type_count++;
  return canon_parse_field_list(parser, true);
}

static bool canon_parse_extern_declaration(CanonParser *parser, const ZCanonicalToken *start) {
  if (canon_accept_word(parser, "type")) return canon_parse_type_declaration(parser, true);
  if (!canon_accept_word(parser, "c")) return canon_fail(parser->diag, canon_peek(parser), "expected extern c or extern type", "extern c", start->text);
  if (canon_peek(parser)->kind != Z_CANON_TOKEN_STRING) return canon_fail(parser->diag, canon_peek(parser), "expected C header string", "string", canon_peek(parser)->text);
  parser->pos++;
  if (!canon_accept_word(parser, "as")) return canon_fail(parser->diag, canon_peek(parser), "expected C import alias", "as", canon_peek(parser)->text);
  if (!canon_expect_word_token(parser, "expected C import alias name")) return false;
  return canon_expect_declaration_end(parser, "unexpected tokens after extern c declaration");
}

static bool canon_parse_enum_declaration(CanonParser *parser) {
  if (!canon_expect_word_token(parser, "expected enum name")) return false;
  if (!canon_parse_type_params(parser)) return false;
  if (parser->facts) parser->facts->enum_count++;
  return canon_parse_field_list(parser, false);
}

static bool canon_parse_choice_declaration(CanonParser *parser) {
  if (!canon_expect_word_token(parser, "expected choice name")) return false;
  if (!canon_parse_type_params(parser)) return false;
  if (parser->facts) parser->facts->choice_count++;
  return canon_parse_field_list(parser, true);
}

static bool canon_parse_alias_declaration(CanonParser *parser) {
  if (!canon_expect_word_token(parser, "expected alias name")) return false;
  if (!canon_expect_symbol(parser, "=", "expected alias target")) return false;
  return canon_parse_expr_line(parser, false);
}

static bool canon_parse_const_declaration(CanonParser *parser) {
  if (!canon_expect_word_token(parser, "expected const name")) return false;
  if (!canon_expect_symbol(parser, ":", "expected const type")) return false;
  if (!canon_parse_type_until(parser, "=", NULL)) return false;
  if (!canon_expect_symbol(parser, "=", "expected const value")) return false;
  return canon_parse_expr_line(parser, false);
}

static bool canon_parse_public_declaration(CanonParser *parser, const ZCanonicalToken *start) {
  if (canon_accept_word(parser, "fn")) {
    if (parser->facts) parser->facts->function_count++;
    return canon_parse_signature(parser, true, 1);
  }
  if (canon_accept_word(parser, "type")) return canon_parse_type_declaration(parser, false);
  if (canon_accept_word(parser, "extern")) return canon_parse_extern_declaration(parser, start);
  if (canon_accept_word(parser, "enum")) return canon_parse_enum_declaration(parser);
  if (canon_accept_word(parser, "choice")) return canon_parse_choice_declaration(parser);
  if (canon_accept_word(parser, "interface")) {
    if (parser->facts) parser->facts->interface_count++;
    return canon_parse_interface(parser);
  }
  if (canon_accept_word(parser, "alias")) return canon_parse_alias_declaration(parser);
  if (canon_accept_word(parser, "const")) return canon_parse_const_declaration(parser);
  return canon_fail(parser->diag, canon_peek(parser), "expected public declaration", "fn, type, extern type, enum, choice, interface, alias, or const", canon_peek(parser) ? canon_peek(parser)->text : "end of file");
}

static bool canon_parse_declaration(CanonParser *parser) {
  canon_skip_newlines(parser);
  const ZCanonicalToken *start = canon_peek(parser);
  if (!start || start->kind == Z_CANON_TOKEN_EOF) return true;
  canon_push_node(parser->tree, (ZCanonicalNode){Z_CANON_NODE_DECL, parser->pos, 0, 0, start->line, start->column});
  if (parser->facts) parser->facts->declaration_count++;
  if (canon_accept_word(parser, "fun")) return canon_fail(parser->diag, start, "function declarations use fn", "fn", "fun");
  if (canon_accept_word(parser, "shape")) return canon_fail(parser->diag, start, "record declarations use type", "type", "shape");
  if (canon_accept_word(parser, "pub")) return canon_parse_public_declaration(parser, start);
  if (canon_accept_word(parser, "export")) {
    if (!canon_accept_word(parser, "c") || !canon_accept_word(parser, "fn")) return canon_fail(parser->diag, canon_peek(parser), "expected export c fn", "export c fn", start->text);
    if (parser->facts) parser->facts->function_count++;
    return canon_parse_signature(parser, true, 1);
  }
  if (canon_accept_word(parser, "fn")) {
    if (parser->facts) parser->facts->function_count++;
    return canon_parse_signature(parser, true, 1);
  }
  if (canon_accept_word(parser, "type")) return canon_parse_type_declaration(parser, false);
  if (canon_accept_word(parser, "extern")) return canon_parse_extern_declaration(parser, start);
  if (canon_accept_word(parser, "enum")) return canon_parse_enum_declaration(parser);
  if (canon_accept_word(parser, "choice")) return canon_parse_choice_declaration(parser);
  if (canon_accept_word(parser, "interface")) {
    if (parser->facts) parser->facts->interface_count++;
    return canon_parse_interface(parser);
  }
  if (canon_accept_word(parser, "test")) {
    if (canon_peek(parser)->kind != Z_CANON_TOKEN_STRING) return canon_fail(parser->diag, canon_peek(parser), "expected test name", "string", canon_peek(parser)->text);
    parser->pos++;
    if (parser->facts) parser->facts->test_count++;
    return canon_parse_block(parser, 1);
  }
  if (canon_accept_word(parser, "use")) return canon_parse_expr_line(parser, false);
  if (canon_accept_word(parser, "alias")) return canon_parse_alias_declaration(parser);
  if (canon_accept_word(parser, "const")) return canon_parse_const_declaration(parser);
  return canon_fail(parser->diag, start, "expected canonical declaration", "declaration", start->text);
}

bool z_canonical_text_parse(const ZCanonicalTokenVec *tokens, ZCanonicalTree *tree, ZCanonicalFacts *facts, ZDiag *diag) {
  if (!tokens || !tree || !facts) return canon_fail(diag, NULL, "missing canonical text parser input", "tokens, tree, facts", "missing");
  *tree = (ZCanonicalTree){0};
  *facts = (ZCanonicalFacts){0};
  CanonParser parser = {.tokens = tokens, .tree = tree, .facts = facts, .diag = diag};
  canon_skip_newlines(&parser);
  while (canon_peek(&parser) && canon_peek(&parser)->kind != Z_CANON_TOKEN_EOF) {
    if (!canon_parse_declaration(&parser)) return false;
    canon_skip_newlines(&parser);
  }
  return true;
}

void z_free_canonical_text_tokens(ZCanonicalTokenVec *tokens) {
  if (!tokens) return;
  for (size_t i = 0; i < tokens->len; i++) free(tokens->items[i].text);
  free(tokens->items);
  *tokens = (ZCanonicalTokenVec){0};
}

void z_free_canonical_text_tree(ZCanonicalTree *tree) {
  if (!tree) return;
  free(tree->items);
  *tree = (ZCanonicalTree){0};
}
