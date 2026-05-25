#include "program_graph_format.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int ok, const char *message);

void *z_checked_malloc(size_t size) {
  void *ptr = malloc(size ? size : 1);
  if (ptr) return ptr;
  fprintf(stderr, "out of memory\n");
  exit(1);
}

void *z_checked_calloc(size_t count, size_t item_size) {
  void *ptr = calloc(count ? count : 1, item_size ? item_size : 1);
  if (ptr) return ptr;
  fprintf(stderr, "out of memory\n");
  exit(1);
}

void *z_checked_reallocarray(void *ptr, size_t count, size_t item_size) {
  void *next = realloc(ptr, (count ? count : 1) * (item_size ? item_size : 1));
  if (next) return next;
  fprintf(stderr, "out of memory\n");
  exit(1);
}

size_t z_grow_capacity(size_t current, size_t required, size_t initial) {
  size_t next = current ? current : initial;
  while (next < required) next *= 2;
  return next;
}

char *z_strdup(const char *text) {
  size_t len = strlen(text ? text : "");
  return z_strndup(text ? text : "", len);
}

char *z_strndup(const char *text, size_t len) {
  char *copy = z_checked_malloc(len + 1);
  memcpy(copy, text ? text : "", len + 1);
  copy[len] = 0;
  return copy;
}

char *z_read_file(const char *path, ZDiag *diag) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    if (diag) snprintf(diag->message, sizeof(diag->message), "failed to read test graph");
    return NULL;
  }
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  rewind(file);
  char *data = z_checked_malloc((size_t)size + 1);
  size_t read = fread(data, 1, (size_t)size, file);
  fclose(file);
  data[read] = 0;
  return data;
}

bool z_write_file(const char *path, const char *text, ZDiag *diag) {
  FILE *file = fopen(path, "wb");
  if (!file) {
    if (diag) snprintf(diag->message, sizeof(diag->message), "failed to write test graph");
    return false;
  }
  fputs(text ? text : "", file);
  fclose(file);
  return true;
}

void zbuf_init(ZBuf *buf) { *buf = (ZBuf){0}; }
void zbuf_free(ZBuf *buf) {
  free(buf->data);
  *buf = (ZBuf){0};
}

void zbuf_append_char(ZBuf *buf, char ch) {
  if (buf->len + 1 >= buf->cap) {
    buf->cap = z_grow_capacity(buf->cap, buf->len + 2, 64);
    buf->data = z_checked_reallocarray(buf->data, buf->cap, 1);
  }
  buf->data[buf->len++] = ch;
  buf->data[buf->len] = 0;
}

void zbuf_append(ZBuf *buf, const char *text) {
  for (const char *p = text ? text : ""; *p; p++) zbuf_append_char(buf, *p);
}

void zbuf_appendf(ZBuf *buf, const char *fmt, ...) {
  char stack[256];
  va_list args;
  va_start(args, fmt);
  int len = vsnprintf(stack, sizeof(stack), fmt, args);
  va_end(args);
  expect(len >= 0 && (size_t)len < sizeof(stack), "formatted test buffer overflow");
  zbuf_append(buf, stack);
}

static void expect(int ok, const char *message) {
  if (ok) return;
  fprintf(stderr, "%s\n", message);
  exit(1);
}

static void set_node(ZProgramGraphNode *node, const char *id, ZProgramGraphNodeKind kind, const char *name, const char *type) {
  node->id = z_strdup(id);
  node->kind = kind;
  node->name = name ? z_strdup(name) : NULL;
  node->type = type ? z_strdup(type) : NULL;
  node->path = z_strdup("program-graph-smoke.0");
  node->line = 1;
  node->column = 1;
}

static void set_edge(ZProgramGraphEdge *edge, const char *from, const char *to, const char *kind, ZProgramGraphEdgeTarget target, size_t order) {
  edge->from = z_strdup(from);
  edge->to = z_strdup(to);
  edge->kind = z_strdup(kind);
  edge->target = target;
  edge->order = order;
}

int main(void) {
  ZProgramGraph graph;
  z_program_graph_init(&graph);
  graph.nodes = z_checked_calloc(2, sizeof(ZProgramGraphNode));
  graph.node_len = 2;
  graph.node_cap = 2;
  set_node(&graph.nodes[0], "node:000001", Z_PROGRAM_GRAPH_NODE_MODULE, "smoke", NULL);
  set_node(&graph.nodes[1], "node:000002", Z_PROGRAM_GRAPH_NODE_FUNCTION, "main", "Void");
  graph.edges = z_checked_calloc(1, sizeof(ZProgramGraphEdge));
  graph.edge_len = 1;
  graph.edge_cap = 1;
  set_edge(&graph.edges[0], "node:000001", "symbol:missing", "function", Z_PROGRAM_GRAPH_EDGE_TARGET_SYMBOL, 0);
  z_program_graph_finalize_identities(&graph);

  ZProgramGraphValidation validation = {0};
  expect(!z_program_graph_validate(&graph, &validation), "missing symbol target validated");
  expect(strcmp(validation.code, "GRF005") == 0, "missing symbol target reported wrong code");
  expect(strcmp(validation.edge_target, "symbol") == 0, "missing symbol target reported wrong domain");

  free(graph.edges[0].to);
  graph.edges[0].to = z_strdup(graph.nodes[1].symbol_id);
  z_program_graph_finalize_identities(&graph);
  expect(z_program_graph_validate(&graph, &validation), "valid symbol target failed validation");
  expect(validation.state == Z_PROGRAM_GRAPH_VALIDATION_SHAPE_VALID, "valid graph reported wrong state");

  ZBuf dump;
  zbuf_init(&dump);
  z_program_graph_append_dump(&dump, &graph, &validation);
  ZProgramGraph parsed;
  ZDiag diag = {0};
  expect(z_program_graph_parse_dump(dump.data, &parsed, &diag), diag.message);
  ZProgramGraphValidation parsed_validation = {0};
  expect(z_program_graph_validate(&parsed, &parsed_validation), "parsed graph failed validation");
  ZBuf redump;
  zbuf_init(&redump);
  z_program_graph_append_dump(&redump, &parsed, &parsed_validation);
  expect(strcmp(dump.data, redump.data) == 0, "parsed graph dump was not byte-stable");

  char storage_path[128];
  snprintf(storage_path, sizeof(storage_path), "/tmp/zero-program-graph-smoke-%p.graph", (void *)&graph);
  ZDiag storage_diag = {0};
  expect(z_program_graph_save(storage_path, &graph, &storage_diag), storage_diag.message);
  ZProgramGraph loaded;
  expect(z_program_graph_load(storage_path, &loaded, &storage_diag), storage_diag.message);
  ZProgramGraphValidation loaded_validation = {0};
  expect(z_program_graph_validate(&loaded, &loaded_validation), "loaded graph failed validation");
  ZBuf loaded_dump;
  zbuf_init(&loaded_dump);
  z_program_graph_append_dump(&loaded_dump, &loaded, &loaded_validation);
  expect(strcmp(dump.data, loaded_dump.data) == 0, "loaded graph dump was not byte-stable");
  zbuf_free(&loaded_dump);
  z_program_graph_free(&loaded);
  remove(storage_path);

  char *saved_hash = parsed.nodes[1].node_hash;
  parsed.nodes[1].node_hash = z_strdup("nodehash:0000000000000000");
  ZBuf corrupted;
  zbuf_init(&corrupted);
  z_program_graph_append_dump(&corrupted, &parsed, &parsed_validation);
  ZProgramGraph rejected;
  ZDiag rejected_diag = {0};
  expect(!z_program_graph_parse_dump(corrupted.data, &rejected, &rejected_diag), "corrupt node hash parsed");
  expect(strstr(rejected_diag.message, "identities") != NULL, "corrupt node hash reported wrong diagnostic");
  free(parsed.nodes[1].node_hash);
  parsed.nodes[1].node_hash = saved_hash;

  ZProgramGraph wrong_schema;
  ZDiag wrong_schema_diag = {0};
  expect(!z_program_graph_parse_dump("zero-program-graph v2\n", &wrong_schema, &wrong_schema_diag), "unknown schema parsed");
  expect(strstr(wrong_schema_diag.message, "schema") != NULL, "unknown schema reported wrong diagnostic");

  const char *failed_dump =
      "zero-program-graph v1\n"
      "canonicalSource false\n"
      "idStrategy \"deterministic-traversal-r0\"\n"
      "moduleIdentity \"module:main\"\n"
      "graphHash \"\"\n"
      "validation \"decoded\" failed\n"
      "diagnostic code=\"GRF001\" message=\"program graph construction failed\"\n"
      "counts nodes=0 edges=0\n";
  ZProgramGraph failed_artifact;
  ZDiag failed_artifact_diag = {0};
  expect(!z_program_graph_parse_dump(failed_dump, &failed_artifact, &failed_artifact_diag), "failed validation artifact parsed");
  expect(strstr(failed_artifact_diag.message, "failed validation") != NULL, "failed validation artifact reported wrong diagnostic");

  ZBuf trailing_extra;
  zbuf_init(&trailing_extra);
  zbuf_append(&trailing_extra, dump.data);
  zbuf_append(&trailing_extra, "\nextra\n");
  ZProgramGraph trailing_artifact;
  ZDiag trailing_artifact_diag = {0};
  expect(!z_program_graph_parse_dump(trailing_extra.data, &trailing_artifact, &trailing_artifact_diag), "trailing content parsed");
  expect(strstr(trailing_artifact_diag.message, "unexpected content") != NULL, "trailing content reported wrong diagnostic");

  zbuf_free(&trailing_extra);
  zbuf_free(&corrupted);
  zbuf_free(&redump);
  z_program_graph_free(&parsed);
  zbuf_free(&dump);
  z_program_graph_free(&graph);
  puts("program graph smoke ok");
  return 0;
}
