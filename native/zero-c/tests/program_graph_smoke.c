#include "program_graph.h"

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
  char *copy = z_checked_malloc(len + 1);
  memcpy(copy, text ? text : "", len + 1);
  return copy;
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
  edge->kind = kind;
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
  expect(z_program_graph_validate(&graph, &validation), "valid symbol target failed validation");
  expect(validation.state == Z_PROGRAM_GRAPH_VALIDATION_SHAPE_VALID, "valid graph reported wrong state");

  z_program_graph_free(&graph);
  puts("program graph smoke ok");
  return 0;
}
