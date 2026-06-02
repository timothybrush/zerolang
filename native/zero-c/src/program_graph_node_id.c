#include "program_graph.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static bool graph_text_eq(const char *left, const char *right) {
  const unsigned char *a = (const unsigned char *)(left ? left : "");
  const unsigned char *b = (const unsigned char *)(right ? right : "");
  while (*a && *b && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static uint64_t graph_hash_text(uint64_t hash, const char *text) {
  const unsigned char *p = (const unsigned char *)(text ? text : "");
  while (*p) {
    hash ^= (uint64_t)*p++;
    hash *= 1099511628211ull;
  }
  hash ^= 0xffu;
  hash *= 1099511628211ull;
  return hash;
}

static uint64_t graph_hash_u64(uint64_t hash, uint64_t value) {
  for (unsigned i = 0; i < 8; i++) {
    hash ^= (value >> (i * 8)) & 0xffu;
    hash *= 1099511628211ull;
  }
  return hash;
}

bool z_program_graph_node_id_valid(const char *id) {
  if (!id || id[0] != '#' || !id[1]) return false;
  for (const char *cursor = id + 1; *cursor; cursor++) {
    unsigned char ch = (unsigned char)*cursor;
    if (!((ch >= 'a' && ch <= 'z') ||
          (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') ||
          ch == '_' || ch == '-' || ch == '.')) {
      return false;
    }
  }
  return true;
}

static const char *graph_node_id_domain(const ZProgramGraphNode *node) {
  if (!node) return "node";
  switch (node->kind) {
    case Z_PROGRAM_GRAPH_NODE_MODULE: return "mod";
    case Z_PROGRAM_GRAPH_NODE_IMPORT: return "imp";
    case Z_PROGRAM_GRAPH_NODE_C_IMPORT: return "cimp";
    case Z_PROGRAM_GRAPH_NODE_CONST:
    case Z_PROGRAM_GRAPH_NODE_TYPE_ALIAS:
    case Z_PROGRAM_GRAPH_NODE_SHAPE:
    case Z_PROGRAM_GRAPH_NODE_INTERFACE:
    case Z_PROGRAM_GRAPH_NODE_ENUM:
    case Z_PROGRAM_GRAPH_NODE_CHOICE:
    case Z_PROGRAM_GRAPH_NODE_FUNCTION:
      return "decl";
    case Z_PROGRAM_GRAPH_NODE_PARAM: return "param";
    case Z_PROGRAM_GRAPH_NODE_FIELD: return "field";
    case Z_PROGRAM_GRAPH_NODE_ENUM_CASE:
    case Z_PROGRAM_GRAPH_NODE_CHOICE_CASE:
      return "case";
    case Z_PROGRAM_GRAPH_NODE_BLOCK: return "block";
    case Z_PROGRAM_GRAPH_NODE_LET:
    case Z_PROGRAM_GRAPH_NODE_ASSIGNMENT:
    case Z_PROGRAM_GRAPH_NODE_DEFER:
    case Z_PROGRAM_GRAPH_NODE_CHECK:
    case Z_PROGRAM_GRAPH_NODE_RETURN:
    case Z_PROGRAM_GRAPH_NODE_EXPRESSION_STATEMENT:
    case Z_PROGRAM_GRAPH_NODE_IF:
    case Z_PROGRAM_GRAPH_NODE_WHILE:
    case Z_PROGRAM_GRAPH_NODE_FOR:
    case Z_PROGRAM_GRAPH_NODE_BREAK:
    case Z_PROGRAM_GRAPH_NODE_CONTINUE:
    case Z_PROGRAM_GRAPH_NODE_MATCH:
    case Z_PROGRAM_GRAPH_NODE_RAISE:
    case Z_PROGRAM_GRAPH_NODE_MATCH_ARM:
    case Z_PROGRAM_GRAPH_NODE_STATEMENT:
      return "stmt";
    case Z_PROGRAM_GRAPH_NODE_TYPE_REF: return "type";
    case Z_PROGRAM_GRAPH_NODE_EFFECT_REF: return "effect";
    case Z_PROGRAM_GRAPH_NODE_ERROR_VARIANT: return "err";
    default:
      return "expr";
  }
}

static bool graph_edge_order_participates(const ZProgramGraphNode *node, const ZProgramGraphEdge *edge) {
  if (!edge) return false;
  if (node && node->kind == Z_PROGRAM_GRAPH_NODE_FUNCTION && graph_text_eq(edge->kind, "function")) return false;
  if (graph_text_eq(edge->kind, "statement")) return false;
  return true;
}

static uint64_t graph_node_id_hash_value(const ZProgramGraph *graph, const ZProgramGraphNode *node, const ZProgramGraphEdge *owner_edge, const char *owner_new_id) {
  uint64_t hash = 1469598103934665603ull;
  hash = graph_hash_text(hash, graph && graph->module_identity ? graph->module_identity : "");
  hash = graph_hash_text(hash, graph_node_id_domain(node));
  hash = graph_hash_text(hash, z_program_graph_node_kind_name(node->kind));
  hash = graph_hash_text(hash, node->type);
  hash = graph_hash_u64(hash, node->is_public ? 1 : 0);
  hash = graph_hash_u64(hash, node->is_mutable ? 1 : 0);
  hash = graph_hash_u64(hash, node->is_static ? 1 : 0);
  hash = graph_hash_u64(hash, node->fallible ? 1 : 0);
  hash = graph_hash_u64(hash, node->export_c ? 1 : 0);
  if (owner_edge) {
    hash = graph_hash_text(hash, owner_new_id);
    hash = graph_hash_text(hash, owner_edge->kind);
    if (graph_edge_order_participates(node, owner_edge)) hash = graph_hash_u64(hash, (uint64_t)owner_edge->order);
  } else if (node->kind == Z_PROGRAM_GRAPH_NODE_MODULE) {
    hash = graph_hash_text(hash, node->name);
  }
  return hash;
}

static bool graph_id_is_used(char **ids, size_t len, const char *id) {
  for (size_t i = 0; i < len; i++) {
    if (ids[i] && id && graph_text_eq(ids[i], id)) return true;
  }
  return false;
}

static size_t graph_find_old_id(char **old_ids, size_t len, const char *id) {
  for (size_t i = 0; old_ids && id && i < len; i++) {
    if (old_ids[i] && graph_text_eq(old_ids[i], id)) return i;
  }
  return SIZE_MAX;
}

static const ZProgramGraphEdge *graph_owner_edge_for_node(const ZProgramGraph *graph, const char *node_id) {
  for (size_t i = 0; graph && node_id && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && graph_text_eq(edge->to, node_id)) return edge;
  }
  return NULL;
}

static char *graph_source_node_id(const ZProgramGraph *graph, const ZProgramGraphNode *node, const ZProgramGraphEdge *owner_edge, const char *owner_new_id, char **ids, size_t id_len) {
  uint64_t hash = graph_node_id_hash_value(graph, node, owner_edge, owner_new_id);
  ZBuf base;
  zbuf_init(&base);
  zbuf_appendf(&base, "#%s_%08llx", graph_node_id_domain(node), (unsigned long long)(hash & 0xffffffffull));
  if (!graph_id_is_used(ids, id_len, base.data)) return base.data ? base.data : z_strdup("#node_00000000");
  uint64_t collision = graph_hash_text(hash, node->id);
  for (size_t attempt = 0;; attempt++) {
    ZBuf unique;
    zbuf_init(&unique);
    zbuf_append(&unique, base.data);
    if (attempt == 0) zbuf_appendf(&unique, "-%04llx", (unsigned long long)(collision & 0xffffull));
    else zbuf_appendf(&unique, "-%04llx-%zu", (unsigned long long)(collision & 0xffffull), attempt);
    if (graph_id_is_used(ids, id_len, unique.data)) {
      zbuf_free(&unique);
      continue;
    }
    zbuf_free(&base);
    return unique.data ? unique.data : z_strdup("#node_00000000");
  }
}

static const char *graph_remapped_id(const char *old_id, char **old_ids, char **new_ids, size_t len) {
  size_t index = graph_find_old_id(old_ids, len, old_id);
  return index == SIZE_MAX ? old_id : new_ids[index];
}

void z_program_graph_assign_source_node_ids(ZProgramGraph *graph) {
  if (!graph || graph->node_len == 0) return;
  char **old_ids = z_checked_calloc(graph->node_len, sizeof(char *));
  char **new_ids = z_checked_calloc(graph->node_len, sizeof(char *));
  for (size_t i = 0; i < graph->node_len; i++) old_ids[i] = z_strdup(graph->nodes[i].id ? graph->nodes[i].id : "");
  for (size_t i = 0; i < graph->node_len; i++) {
    const ZProgramGraphEdge *owner_edge = graph_owner_edge_for_node(graph, graph->nodes[i].id);
    const char *owner_new_id = NULL;
    if (owner_edge) {
      size_t owner_index = graph_find_old_id(old_ids, i, owner_edge->from);
      if (owner_index != SIZE_MAX) owner_new_id = new_ids[owner_index];
    }
    new_ids[i] = graph_source_node_id(graph, &graph->nodes[i], owner_edge, owner_new_id, new_ids, i);
  }
  for (size_t i = 0; i < graph->node_len; i++) {
    free(graph->nodes[i].id);
    graph->nodes[i].id = z_strdup(new_ids[i]);
  }
  for (size_t i = 0; i < graph->edge_len; i++) {
    const char *from = graph_remapped_id(graph->edges[i].from, old_ids, new_ids, graph->node_len);
    free(graph->edges[i].from);
    graph->edges[i].from = z_strdup(from);
    if (graph->edges[i].target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE) {
      const char *to = graph_remapped_id(graph->edges[i].to, old_ids, new_ids, graph->node_len);
      free(graph->edges[i].to);
      graph->edges[i].to = z_strdup(to);
    }
  }
  for (size_t i = 0; i < graph->node_len; i++) {
    free(old_ids[i]);
    free(new_ids[i]);
  }
  free(old_ids);
  free(new_ids);
}
