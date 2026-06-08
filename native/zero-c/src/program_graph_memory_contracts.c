#include "program_graph_contracts.h"

#include <stdio.h>
#include <string.h>

static bool memory_text_eq(const char *left, const char *right) { return strcmp(left ? left : "", right ? right : "") == 0; }

static bool memory_text_starts(const char *text, const char *prefix) { return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0; }

static const ZProgramGraphNode *memory_node_by_id(const ZProgramGraph *graph, const char *id) {
  for (size_t i = 0; graph && id && i < graph->node_len; i++) {
    if (memory_text_eq(graph->nodes[i].id, id)) return &graph->nodes[i];
  }
  return NULL;
}

static const ZProgramGraphNode *memory_child(const ZProgramGraph *graph, const ZProgramGraphNode *node, const char *kind, size_t order) {
  for (size_t i = 0; graph && node && kind && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && edge->order == order && memory_text_eq(edge->from, node->id) && memory_text_eq(edge->kind, kind)) {
      return memory_node_by_id(graph, edge->to);
    }
  }
  return NULL;
}

static const ZProgramGraphEdge *memory_owner_edge(const ZProgramGraph *graph, const char *node_id) {
  for (size_t i = 0; graph && node_id && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && memory_text_eq(edge->to, node_id)) return edge;
  }
  return NULL;
}

static const ZProgramGraphResolutionReference *memory_ref_for_node(const ZProgramGraphResolutionFacts *resolution, const ZProgramGraphNode *node, const char *kind) {
  for (size_t i = 0; resolution && node && i < resolution->reference_len; i++) {
    const ZProgramGraphResolutionReference *ref = &resolution->references[i];
    if ((!kind || memory_text_eq(ref->kind, kind)) && memory_text_eq(ref->node_id, node->id)) return ref;
  }
  return NULL;
}

static bool memory_maybe_binding(const ZProgramGraph *graph, const ZProgramGraphResolutionFacts *resolution, const ZProgramGraphNode *subject) {
  const ZProgramGraphResolutionReference *ref = memory_ref_for_node(resolution, subject, "identifier");
  const ZProgramGraphNode *target = ref && ref->resolved ? memory_node_by_id(graph, ref->target_node) : NULL;
  return target && memory_text_starts(target->type, "Maybe<");
}

static bool memory_subject_matches(const ZProgramGraphNode *subject, const char *name) { return subject && subject->kind == Z_PROGRAM_GRAPH_NODE_IDENTIFIER && memory_text_eq(subject->name, name); }

static bool memory_block_has_direct_return(const ZProgramGraph *graph, const ZProgramGraphNode *block) {
  for (size_t i = 0; graph && block && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target != Z_PROGRAM_GRAPH_EDGE_TARGET_NODE || !memory_text_eq(edge->from, block->id) || !memory_text_eq(edge->kind, "statement")) continue;
    const ZProgramGraphNode *stmt = memory_node_by_id(graph, edge->to);
    if (stmt && stmt->kind == Z_PROGRAM_GRAPH_NODE_RETURN) return true;
  }
  return false;
}

static bool memory_expr_is_false(const ZProgramGraphNode *expr) { return expr && expr->kind == Z_PROGRAM_GRAPH_NODE_LITERAL && memory_text_eq(expr->value, "false"); }

static bool memory_expr_is_has_access_for(const ZProgramGraph *graph, const ZProgramGraphNode *expr, const char *subject_name) {
  return expr && expr->kind == Z_PROGRAM_GRAPH_NODE_FIELD_ACCESS && memory_text_eq(expr->name, "has") &&
         memory_subject_matches(memory_child(graph, expr, "left", 0), subject_name);
}

static bool memory_expr_has_guard_for(const ZProgramGraph *graph, const ZProgramGraphNode *expr, const char *subject_name, size_t depth) {
  if (!graph || !expr || !subject_name || depth > graph->node_len) return false;
  if (memory_expr_is_has_access_for(graph, expr, subject_name)) return true;
  for (size_t i = 0; i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && memory_text_eq(edge->from, expr->id)) {
      if (memory_expr_has_guard_for(graph, memory_node_by_id(graph, edge->to), subject_name, depth + 1)) return true;
    }
  }
  return false;
}

static bool memory_block_has_then_guard(const ZProgramGraph *graph, const ZProgramGraphNode *block, const char *subject_name) {
  const ZProgramGraphEdge *block_owner = block ? memory_owner_edge(graph, block->id) : NULL;
  if (!block_owner || !memory_text_eq(block_owner->kind, "then")) return false;
  const ZProgramGraphNode *if_node = memory_node_by_id(graph, block_owner->from);
  if (!if_node || if_node->kind != Z_PROGRAM_GRAPH_NODE_IF) return false;
  return memory_expr_has_guard_for(graph, memory_child(graph, if_node, "expr", 0), subject_name, 0);
}

static bool memory_expr_negative_has_guard_for(const ZProgramGraph *graph, const ZProgramGraphNode *expr, const char *subject_name) {
  if (!expr || expr->kind != Z_PROGRAM_GRAPH_NODE_CALL) return false;
  const ZProgramGraphNode *left = memory_child(graph, expr, "left", 0);
  const ZProgramGraphNode *right = memory_child(graph, expr, "right", 1);
  if (memory_text_eq(expr->name, "||")) {
    return memory_expr_negative_has_guard_for(graph, left, subject_name) || memory_expr_negative_has_guard_for(graph, right, subject_name);
  }
  return memory_text_eq(expr->name, "==") &&
         ((memory_expr_is_has_access_for(graph, left, subject_name) && memory_expr_is_false(right)) ||
          (memory_expr_is_false(left) && memory_expr_is_has_access_for(graph, right, subject_name)));
}

static bool memory_if_proves_subject_after(const ZProgramGraph *graph, const ZProgramGraphNode *stmt, const char *subject_name) {
  if (!stmt || stmt->kind != Z_PROGRAM_GRAPH_NODE_IF) return false;
  if (!memory_expr_negative_has_guard_for(graph, memory_child(graph, stmt, "expr", 0), subject_name)) return false;
  return memory_block_has_direct_return(graph, memory_child(graph, stmt, "then", 0));
}

static bool memory_prior_negative_return_guarded_in_block(const ZProgramGraph *graph, const ZProgramGraphNode *block, size_t order, const char *subject_name) {
  for (size_t i = 0; graph && block && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target != Z_PROGRAM_GRAPH_EDGE_TARGET_NODE || edge->order >= order || !memory_text_eq(edge->from, block->id) || !memory_text_eq(edge->kind, "statement")) continue;
    if (memory_if_proves_subject_after(graph, memory_node_by_id(graph, edge->to), subject_name)) return true;
  }
  return false;
}

static bool memory_prior_negative_return_guarded(const ZProgramGraph *graph, const ZProgramGraphNode *read, const char *subject_name) {
  const char *current = read ? read->id : NULL;
  for (size_t depth = 0; graph && current && depth < graph->node_len; depth++) {
    const ZProgramGraphEdge *owner = memory_owner_edge(graph, current);
    const ZProgramGraphNode *owner_node = owner ? memory_node_by_id(graph, owner->from) : NULL;
    if (!owner_node) return false;
    if (owner_node->kind == Z_PROGRAM_GRAPH_NODE_BLOCK && memory_text_eq(owner->kind, "statement") &&
        memory_prior_negative_return_guarded_in_block(graph, owner_node, owner->order, subject_name)) return true;
    current = owner_node->id;
  }
  return false;
}

static bool memory_short_circuit_guarded(const ZProgramGraph *graph, const ZProgramGraphNode *read, const char *subject_name) {
  const char *current = read ? read->id : NULL;
  for (size_t depth = 0; graph && current && depth < graph->node_len; depth++) {
    const ZProgramGraphEdge *owner = memory_owner_edge(graph, current);
    const ZProgramGraphNode *parent = owner ? memory_node_by_id(graph, owner->from) : NULL;
    if (!parent) return false;
    if (parent->kind == Z_PROGRAM_GRAPH_NODE_CALL && memory_text_eq(owner->kind, "right")) {
      const ZProgramGraphNode *left = memory_child(graph, parent, "left", 0);
      if (memory_text_eq(parent->name, "&&") && memory_expr_has_guard_for(graph, left, subject_name, 0)) return true;
      if (memory_text_eq(parent->name, "||") && memory_expr_negative_has_guard_for(graph, left, subject_name)) return true;
    }
    current = parent->id;
  }
  return false;
}

static bool memory_value_read_guarded(const ZProgramGraph *graph, const ZProgramGraphNode *read, const char *subject_name) {
  if (memory_short_circuit_guarded(graph, read, subject_name)) return true;
  if (memory_prior_negative_return_guarded(graph, read, subject_name)) return true;
  const char *current = read ? read->id : NULL;
  for (size_t depth = 0; graph && current && depth < graph->node_len; depth++) {
    const ZProgramGraphEdge *owner = memory_owner_edge(graph, current);
    const ZProgramGraphNode *owner_node = owner ? memory_node_by_id(graph, owner->from) : NULL;
    if (!owner_node) return false;
    if (owner_node->kind == Z_PROGRAM_GRAPH_NODE_BLOCK && memory_block_has_then_guard(graph, owner_node, subject_name)) return true;
    current = owner_node->id;
  }
  return false;
}

static bool fail_maybe_value_read(const ZProgramGraphNode *read, const ZProgramGraphNode *subject, const char *path, ZDiag *diag) {
  if (diag) {
    *diag = (ZDiag){0};
    diag->code = 3051;
    diag->path = read && read->path && read->path[0] ? read->path : path;
    diag->line = read && read->line > 0 ? read->line : 1;
    diag->column = read && read->column > 0 ? read->column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "Maybe payload read requires a presence guard");
    snprintf(diag->expected, sizeof(diag->expected), ".has proven true, check maybe_value, or rescue maybe_value err fallback");
    snprintf(diag->actual, sizeof(diag->actual), "%s", subject && subject->name ? subject->name : "temporary Maybe value");
    snprintf(diag->help, sizeof(diag->help), "guard the Maybe with `.has` before reading `.value`, or use `check`/`rescue` to handle absence");
  }
  return false;
}

bool z_program_graph_memory_contracts_ok(const ZProgramGraph *graph, const ZProgramGraphResolutionFacts *resolution, const char *path, ZDiag *diag) {
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_FIELD_ACCESS || !memory_text_eq(node->name, "value")) continue;
    const ZProgramGraphNode *subject = memory_child(graph, node, "left", 0);
    if (!memory_maybe_binding(graph, resolution, subject)) continue;
    if (!memory_value_read_guarded(graph, node, subject ? subject->name : "")) return fail_maybe_value_read(node, subject, path, diag);
  }
  return true;
}
