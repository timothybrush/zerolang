#include "canonical_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *z_checked_malloc(size_t size) {
  void *ptr = malloc(size ? size : 1);
  if (!ptr) abort();
  return ptr;
}

void *z_checked_reallocarray(void *ptr, size_t count, size_t item_size) {
  if (item_size && count > ((size_t)-1) / item_size) abort();
  void *next = realloc(ptr, count * item_size);
  if (!next && count) abort();
  return next;
}

size_t z_grow_capacity(size_t current, size_t required, size_t initial) {
  size_t next = current ? current : initial;
  while (next < required) next *= 2;
  return next;
}

char *z_strndup(const char *text, size_t len) {
  char *copy = z_checked_malloc(len + 1);
  memcpy(copy, text, len);
  copy[len] = 0;
  return copy;
}

char *z_strdup(const char *text) {
  size_t len = strlen(text ? text : "");
  char *copy = z_checked_malloc(len + 1);
  memcpy(copy, text ? text : "", len + 1);
  return copy;
}

static void expect(bool ok, const char *message) {
  if (ok) return;
  fprintf(stderr, "%s\n", message);
  exit(1);
}

static char *read_text_file(const char *path) {
  FILE *file = fopen(path, "rb");
  expect(file != NULL, "failed to open fixture");
  expect(fseek(file, 0, SEEK_END) == 0, "failed to seek fixture");
  long size = ftell(file);
  expect(size >= 0, "failed to size fixture");
  rewind(file);
  char *text = malloc((size_t)size + 1);
  expect(text != NULL, "failed to allocate fixture");
  size_t read = fread(text, 1, (size_t)size, file);
  expect(read == (size_t)size, "failed to read fixture");
  text[size] = 0;
  fclose(file);
  return text;
}

static bool parse_source(const char *source, ZCanonicalFacts *facts, ZDiag *diag) {
  ZCanonicalTokenVec tokens = z_canonical_text_tokenize(source, diag);
  if (diag->code != 0) {
    z_free_canonical_text_tokens(&tokens);
    return false;
  }
  ZCanonicalTree tree = {0};
  bool ok = z_canonical_text_parse(&tokens, &tree, facts, diag);
  z_free_canonical_text_tree(&tree);
  z_free_canonical_text_tokens(&tokens);
  return ok;
}

static void expect_accepts(const char *source, const char *label) {
  ZDiag diag = {0};
  ZCanonicalFacts facts = {0};
  if (parse_source(source, &facts, &diag)) return;
  fprintf(stderr, "%s:%d:%d: %s\n", label, diag.line, diag.column, diag.message);
  exit(1);
}

static void expect_rejects(const char *source, const char *label) {
  ZDiag diag = {0};
  ZCanonicalFacts facts = {0};
  if (!parse_source(source, &facts, &diag) && diag.code == 100) return;
  fprintf(stderr, "%s: expected canonical text rejection\n", label);
  exit(1);
}

static void parses_declarations_and_blocks(void) {
  const char *source =
    "type Point {\n"
    "    x: i32,\n"
    "    y: i32,\n"
    "}\n"
    "\n"
    "fn sum(point: Point) -> i32 {\n"
    "    return point.x + point.y\n"
    "}\n"
    "\n"
    "pub fn main(world: World) -> Void raises {\n"
    "    let point: Point = Point { x: 40, y: 2 }\n"
    "    let total: i32 = sum(point)\n"
    "    if total == 42 {\n"
    "        check world.out.write(\"point works\\n\")\n"
    "    }\n"
    "}\n";
  ZDiag diag = {0};
  ZCanonicalFacts facts = {0};
  expect(parse_source(source, &facts, &diag), diag.message);
  expect(facts.declaration_count == 3, "expected three declarations");
  expect(facts.type_count == 1, "expected one type declaration");
  expect(facts.function_count == 2, "expected two functions");
  expect(facts.block_count == 3, "expected function and if blocks");
  expect(facts.max_block_depth == 2, "expected nested block depth");
}

static void parses_fallibility_choices_and_interfaces(void) {
  const char *source =
    "choice Result {\n"
    "    ok: i32,\n"
    "    err: String,\n"
    "}\n"
    "\n"
    "interface Id {\n"
    "    fn id<X>(x: X) -> X\n"
    "}\n"
    "\n"
    "fn validate(ok: Bool) -> i32 raises [InvalidInput] {\n"
    "    if ok == false {\n"
    "        raise InvalidInput\n"
    "    }\n"
    "    return 42\n"
    "}\n";
  ZDiag diag = {0};
  ZCanonicalFacts facts = {0};
  expect(parse_source(source, &facts, &diag), diag.message);
  expect(facts.choice_count == 1, "expected choice declaration");
  expect(facts.interface_count == 1, "expected interface declaration");
  expect(facts.function_count == 1, "expected concrete function");
}

static void parses_nested_generic_type_commas(void) {
  const char *source =
    "type Pair<T, U> {\n"
    "    left: T,\n"
    "    right: U,\n"
    "}\n"
    "\n"
    "fn takes_pair(pair: Pair<i32, u8>) -> Void {\n"
    "    return\n"
    "}\n";
  expect_accepts(source, "nested generic type commas");
}

static void parses_separate_boolean_comparisons(void) {
  const char *source =
    "fn compare(a: i32, b: i32, c: i32, d: i32) -> Void {\n"
    "    if a < b && c < d {\n"
    "        return\n"
    "    }\n"
    "}\n";
  expect_accepts(source, "separate boolean comparisons");
}

static void parses_parenthesized_comparisons(void) {
  const char *source =
    "fn compare(a: i32, b: i32, expected: Bool) -> Void {\n"
    "    let same: Bool = (a < b) == expected\n"
    "}\n";
  expect_accepts(source, "parenthesized comparison");
}

static void records_block_open_locations(void) {
  const char *source =
    "fn main() -> Void {\n"
    "    return\n"
    "}\n";
  ZDiag diag = {0};
  ZCanonicalFacts facts = {0};
  ZCanonicalTokenVec tokens = z_canonical_text_tokenize(source, &diag);
  expect(diag.code == 0, diag.message);
  ZCanonicalTree tree = {0};
  expect(z_canonical_text_parse(&tokens, &tree, &facts, &diag), diag.message);
  bool found = false;
  for (size_t i = 0; i < tree.len; i++) {
    if (tree.items[i].kind != Z_CANON_NODE_BLOCK) continue;
    found = true;
    expect(tree.items[i].line == 1, "expected block node at opening brace line");
    expect(tree.items[i].column == 19, "expected block node at opening brace column");
    expect(tokens.items[tree.items[i].first_token].line == tree.items[i].line, "expected block token line to match node");
    expect(tokens.items[tree.items[i].first_token].column == tree.items[i].column, "expected block token column to match node");
    break;
  }
  expect(found, "expected block node");
  z_free_canonical_text_tree(&tree);
  z_free_canonical_text_tokens(&tokens);
}

static void parses_public_declarations_and_extern_types(void) {
  const char *source =
    "pub extern type CPoint\n"
    "\n"
    "pub type Point {\n"
    "    x: i32,\n"
    "}\n"
    "\n"
    "pub const answer: i32 = 42\n"
    "pub alias Count = i32\n"
    "\n"
    "pub interface Reader {\n"
    "    fn read(self: Self) -> i32\n"
    "}\n";
  ZDiag diag = {0};
  ZCanonicalFacts facts = {0};
  expect(parse_source(source, &facts, &diag), diag.message);
  expect(facts.declaration_count == 5, "expected public declarations");
  expect(facts.type_count == 1, "expected public concrete type declaration");
  expect(facts.interface_count == 1, "expected public interface declaration");
}

static void parses_character_literals(void) {
  const char *source =
    "fn chars() -> Void {\n"
    "    let letter: char = 'a'\n"
    "    let newline: char = '\\n'\n"
    "    let hex: char = '\\x41'\n"
    "}\n";
  expect_accepts(source, "character literals");
}

static void parses_empty_return_but_not_empty_checks(void) {
  expect_accepts("fn ok() -> Void {\n    return\n}\n", "empty return");
  expect_rejects("fn bad() -> Void {\n    check\n}\n", "empty check");
  expect_rejects("fn bad() -> Void {\n    expect\n}\n", "empty expect");
}

static void rejects_noncanonical_spellings(void) {
  expect_rejects("fun main() -> Void {}\n", "fun keyword");
  expect_rejects("shape Point {\n    x: i32,\n}\n", "shape keyword");
  expect_rejects("pub fn main(world: World) -> Void raises {\n    let value = 1\n}\n", "missing local type");
  expect_rejects("pub fn main(world: World) -> Void raises {\n    mut value: i32 = 1\n}\n", "mut keyword");
  expect_rejects("type Point {\n    x: i32\n    y: i32\n}\n", "missing commas");
  expect_rejects("fn load() -> Void ! {}\n", "bang fallibility");
  expect_rejects("fn load() -> Void raises { IoError } {}\n", "brace errors");
  expect_rejects("pub fn main(world: World) -> Void raises {\n    check world.out.write \"bad\\n\"\n}\n", "space call");
  expect_rejects("pub fn main(world: World) -> Void raises {\n    let ok: Bool = 1 < 2 < 3\n}\n", "chained comparison");
  expect_rejects("pub fn main(world: World) -> Void raises {\n    let ok: Bool = a == b == c\n}\n", "chained equality comparison");
  expect_rejects("fn bad(items: Items) -> Void {\n    for item in items all {\n        return\n    }\n}\n", "space call in for range");
  expect_rejects("fn bad(items: Items) -> Void {\n    for item in 1 < 2 < 3 {\n        return\n    }\n}\n", "chained comparison in for range");
  expect_rejects("type Pair<T U> {\n    left: T,\n    right: U,\n}\n", "missing type parameter comma");
  expect_rejects("fn id<T U>(value: T) -> T {\n    return value\n}\n", "missing function type parameter comma");
  expect_rejects("fn missing_initializer() -> Void {\n    let value: i32 = // missing\n}\n", "comment-only initializer");
  expect_rejects("type Point {\n    x: i32\n    y: i32,\n}\n", "missing field comma before later comma");
  expect_rejects("fn bad(a: i32\n    b: i32) -> Void {\n    return\n}\n", "missing parameter comma before close");
  expect_rejects("fn bad(a: i32 b: i32) -> Void {\n    return\n}\n", "missing same-line parameter comma");
  expect_rejects("type Point {\n    x: i32 y: i32,\n}\n", "missing same-line field comma");
  expect_rejects("fn bad() -> Void {\n    let value: i32 label = 1\n}\n", "extra local type token");
  expect_rejects("fn bad() -> Void Label {\n    return\n}\n", "extra return type token");
  expect_rejects("fn bad() -> Void {\n    break extra\n}\n", "trailing break tokens");
  expect_rejects("fn bad() -> Void {\n    continue extra\n}\n", "trailing continue tokens");
  expect_rejects("fn bad() -> Void raises {\n    raise InvalidInput extra\n}\n", "trailing raise tokens");
  expect_rejects("fn bad() -> Void {\n    let value: i32 = +\n}\n", "operandless operator");
  expect_rejects("fn bad() -> Void {\n    let value: i32 = ;\n}\n", "unexpected expression punctuation");
  expect_rejects("fn bad() -> Void {\n    let value: i32 = 1 +\n}\n", "trailing expression operator");
  expect_rejects("fn bad() -> Void {\n    let value: char = ''\n}\n", "empty character literal");
  expect_rejects("fn bad() -> Void {\n    let value: char = 'ab'\n}\n", "wide character literal");
  expect_rejects("fn bad() -> Void {\n    let value: char = '\\q'\n}\n", "invalid character escape");
}

static void parse_file_arg(const char *mode, const char *path) {
  char *source = read_text_file(path);
  if (strcmp(mode, "--accept") == 0) expect_accepts(source, path);
  else if (strcmp(mode, "--reject") == 0) expect_rejects(source, path);
  else expect(false, "unknown fixture mode");
  free(source);
}

int main(int argc, char **argv) {
  parses_declarations_and_blocks();
  parses_fallibility_choices_and_interfaces();
  parses_nested_generic_type_commas();
  parses_separate_boolean_comparisons();
  parses_parenthesized_comparisons();
  records_block_open_locations();
  parses_public_declarations_and_extern_types();
  parses_character_literals();
  parses_empty_return_but_not_empty_checks();
  rejects_noncanonical_spellings();
  for (int i = 1; i + 1 < argc; i += 2) parse_file_arg(argv[i], argv[i + 1]);
  printf("canonical text smoke ok\n");
  return 0;
}
