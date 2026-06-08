#include "program_graph_contracts.h"

#include "std_sig.h"

#include <stdio.h>
#include <string.h>

static bool effect_text_eq(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}

static bool effect_text_present(const char *text) {
  return text && text[0];
}

static const ZProgramGraphNode *effect_node_by_id(const ZProgramGraph *graph, const char *id) {
  for (size_t i = 0; graph && id && i < graph->node_len; i++) {
    if (effect_text_eq(graph->nodes[i].id, id)) return &graph->nodes[i];
  }
  return NULL;
}

static const ZProgramGraphResolutionReference *effect_ref_for_node(const ZProgramGraphResolutionFacts *resolution, const ZProgramGraphNode *node, const char *kind) {
  for (size_t i = 0; resolution && node && i < resolution->reference_len; i++) {
    const ZProgramGraphResolutionReference *ref = &resolution->references[i];
    if ((!kind || effect_text_eq(ref->kind, kind)) && effect_text_eq(ref->node_id, node->id)) return ref;
  }
  return NULL;
}

static const ZProgramGraphResolutionReference *effect_call_ref(const ZProgramGraphResolutionFacts *resolution, const ZProgramGraphNode *call) {
  return effect_ref_for_node(resolution, call, "call");
}

static const ZProgramGraphEdge *effect_owner_edge(const ZProgramGraph *graph, const char *node_id) {
  for (size_t i = 0; graph && node_id && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && effect_text_eq(edge->to, node_id)) return edge;
  }
  return NULL;
}

static bool effect_is_under_kind(const ZProgramGraph *graph, const ZProgramGraphNode *node, ZProgramGraphNodeKind kind) {
  const char *current = node ? node->id : NULL;
  for (size_t depth = 0; graph && current && depth < graph->node_len; depth++) {
    const ZProgramGraphEdge *owner = effect_owner_edge(graph, current);
    const ZProgramGraphNode *owner_node = owner ? effect_node_by_id(graph, owner->from) : NULL;
    if (!owner_node) return false;
    if (owner_node->kind == kind) return true;
    current = owner_node->id;
  }
  return false;
}

static const ZProgramGraphNode *effect_child(const ZProgramGraph *graph, const ZProgramGraphNode *node, const char *kind, size_t order) {
  for (size_t i = 0; graph && node && kind && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && edge->order == order && effect_text_eq(edge->from, node->id) && effect_text_eq(edge->kind, kind)) {
      return effect_node_by_id(graph, edge->to);
    }
  }
  return NULL;
}

static const ZProgramGraphNode *effect_owner_function(const ZProgramGraph *graph, const ZProgramGraphNode *node) {
  const char *current = node ? node->id : NULL;
  for (size_t depth = 0; graph && current && depth < graph->node_len; depth++) {
    const ZProgramGraphEdge *owner = effect_owner_edge(graph, current);
    const ZProgramGraphNode *owner_node = owner ? effect_node_by_id(graph, owner->from) : NULL;
    if (!owner_node) return NULL;
    if (owner_node->kind == Z_PROGRAM_GRAPH_NODE_FUNCTION) return owner_node;
    current = owner_node->id;
  }
  return NULL;
}

static bool effect_ref_kind(const ZProgramGraphResolutionReference *ref, const char *kind) {
  return ref && effect_text_eq(ref->target_kind, kind);
}

static bool effect_call_fallible(const ZProgramGraph *graph, const ZProgramGraphResolutionFacts *resolution, const ZProgramGraphNode *call, const ZProgramGraphResolutionReference **ref_out) {
  const ZProgramGraphResolutionReference *ref = effect_call_ref(resolution, call);
  if (ref_out) *ref_out = ref;
  if (!ref || !ref->resolved) return false;
  if (effect_text_eq(ref->qualified_name, "world.out.write")) return true;
  const ZProgramGraphNode *target = effect_node_by_id(graph, ref->target_node);
  if (target && target->kind == Z_PROGRAM_GRAPH_NODE_FUNCTION && target->fallible) return true;
  if (effect_ref_kind(ref, "stdlib") || effect_ref_kind(ref, "graphBackedStdlib")) {
    const ZStdHelperInfo *helper = z_std_helper_find(ref->qualified_name);
    return helper && z_std_helper_is_fallible(helper);
  }
  return false;
}

static const char *effect_literal_type(const ZProgramGraphNode *node, char *scratch, size_t scratch_len) {
  const char *value = node ? node->value : NULL;
  if (effect_text_eq(value, "true") || effect_text_eq(value, "false")) return "Bool";
  if (value && value[0] == '"') return "String";
  if (value && (value[0] == '-' || (value[0] >= '0' && value[0] <= '9'))) {
    if (strstr(value, "_u8")) return "u8";
    if (strstr(value, "_u32")) return "u32";
    if (scratch && scratch_len > 0) snprintf(scratch, scratch_len, "i32");
    return scratch;
  }
  return "";
}

static const char *effect_expr_type(const ZProgramGraph *graph, const ZProgramGraphResolutionFacts *resolution, const ZProgramGraphNode *expr, char *scratch, size_t scratch_len) {
  if (!expr) return "";
  if (effect_text_present(expr->type)) return expr->type;
  if (expr->kind == Z_PROGRAM_GRAPH_NODE_LITERAL) return effect_literal_type(expr, scratch, scratch_len);
  const ZProgramGraphResolutionReference *ref = effect_ref_for_node(resolution, expr, NULL);
  const ZProgramGraphNode *target = ref ? effect_node_by_id(graph, ref->target_node) : NULL;
  if (target && effect_text_present(target->type)) return target->type;
  if (ref && (effect_ref_kind(ref, "stdlib") || effect_ref_kind(ref, "graphBackedStdlib"))) {
    const ZStdHelperInfo *helper = z_std_helper_find(ref->qualified_name);
    return helper && helper->return_type ? helper->return_type : "";
  }
  return "";
}

static bool fail_wrong_return_type(const ZProgramGraphNode *stmt, const char *expected, const char *actual, const char *path, ZDiag *diag) {
  if (diag) {
    *diag = (ZDiag){0};
    diag->code = 3007;
    diag->path = stmt && stmt->path && stmt->path[0] ? stmt->path : path;
    diag->line = stmt && stmt->line > 0 ? stmt->line : 1;
    diag->column = stmt && stmt->column > 0 ? stmt->column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "return type does not match function return type");
    snprintf(diag->expected, sizeof(diag->expected), "%s", expected && expected[0] ? expected : "Void");
    snprintf(diag->actual, sizeof(diag->actual), "%s", actual && actual[0] ? actual : "Void");
    snprintf(diag->help, sizeof(diag->help), "return a value compatible with the function signature");
  }
  return false;
}

static bool fail_missing_return_value(const ZProgramGraphNode *fun, const char *path, ZDiag *diag) {
  if (diag) {
    *diag = (ZDiag){0};
    diag->code = 3007;
    diag->path = fun && fun->path && fun->path[0] ? fun->path : path;
    diag->line = fun && fun->line > 0 ? fun->line : 1;
    diag->column = 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "non-void function must return a value on every path");
    snprintf(diag->expected, sizeof(diag->expected), "%s", fun && fun->type ? fun->type : "non-Void");
    snprintf(diag->actual, sizeof(diag->actual), "function body may fall through");
    snprintf(diag->help, sizeof(diag->help), "add explicit `ret` or `raise` on every path");
  }
  return false;
}

static bool effect_maybe_accepts_present_value(const char *expected, const char *actual) { const char *prefix = "Maybe<"; size_t prefix_len = strlen(prefix), expected_len = expected ? strlen(expected) : 0; if (!expected || !actual || strncmp(expected, prefix, prefix_len) != 0 || expected_len <= prefix_len + 1 || expected[expected_len - 1] != '>') return false; size_t inner_len = expected_len - prefix_len - 1; return strlen(actual) == inner_len && strncmp(expected + prefix_len, actual, inner_len) == 0; }

static bool effect_function_has_return(const ZProgramGraph *graph, const ZProgramGraphNode *fun) {
  for (size_t i = 0; graph && fun && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind == Z_PROGRAM_GRAPH_NODE_RETURN && effect_owner_function(graph, node) == fun) return true;
  }
  return false;
}

static bool effect_function_return_contract_ok(const ZProgramGraph *graph, const ZProgramGraphNode *fun, const char *path, ZDiag *diag) {
  if (!fun || !effect_text_present(fun->type) || effect_text_eq(fun->type, "Void")) return true;
  const ZProgramGraphEdge *owner = effect_owner_edge(graph, fun->id);
  const ZProgramGraphNode *parent = owner ? effect_node_by_id(graph, owner->from) : NULL;
  if (parent && parent->kind == Z_PROGRAM_GRAPH_NODE_INTERFACE && effect_text_eq(owner->kind, "method")) return true;
  if (effect_function_has_return(graph, fun)) return true;
  return fail_missing_return_value(fun, path, diag);
}

static bool effect_return_contract_ok(const ZProgramGraph *graph, const ZProgramGraphResolutionFacts *resolution, const ZProgramGraphNode *stmt, const char *path, ZDiag *diag) {
  const ZProgramGraphNode *fun = effect_owner_function(graph, stmt);
  const char *expected = fun && effect_text_present(fun->type) ? fun->type : "Void";
  const ZProgramGraphNode *expr = effect_child(graph, stmt, "expr", 0);
  char actual_scratch[64] = {0};
  const char *actual = effect_expr_type(graph, resolution, expr, actual_scratch, sizeof(actual_scratch));
  if (!expr) actual = "Void";
  if (!effect_text_present(actual) || effect_text_eq(expected, actual)) return true;
  if (effect_maybe_accepts_present_value(expected, actual)) return true;
  return fail_wrong_return_type(stmt, expected, actual, path, diag);
}

static bool effect_call_checked(const ZProgramGraph *graph, const ZProgramGraphNode *call) {
  return effect_is_under_kind(graph, call, Z_PROGRAM_GRAPH_NODE_CHECK) ||
         effect_is_under_kind(graph, call, Z_PROGRAM_GRAPH_NODE_RESCUE);
}

static bool fail_unchecked_fallible_call(const ZProgramGraphNode *call, const ZProgramGraphResolutionReference *ref, const char *path, ZDiag *diag) {
  if (diag) {
    const char *name = ref && ref->qualified_name && ref->qualified_name[0] ? ref->qualified_name : (call && call->name ? call->name : "");
    *diag = (ZDiag){0};
    diag->code = 1003;
    diag->path = call && call->path && call->path[0] ? call->path : path;
    diag->line = call && call->line > 0 ? call->line : 1;
    diag->column = call && call->column > 0 ? call->column : 1;
    diag->length = 1;
    snprintf(diag->message, sizeof(diag->message), "fallible function call must be checked");
    snprintf(diag->expected, sizeof(diag->expected), "check fallible_call ...");
    snprintf(diag->actual, sizeof(diag->actual), "call to '%s'", name);
    snprintf(diag->help, sizeof(diag->help), "prefix the call with check in a function marked with `raises`");
  }
  return false;
}

bool z_program_graph_effect_contracts_ok(const ZProgramGraph *graph, const ZProgramGraphResolutionFacts *resolution, const char *path, ZDiag *diag) {
  for (size_t i = 0; graph && i < graph->node_len; i++) {
    const ZProgramGraphNode *node = &graph->nodes[i];
    if (node->kind == Z_PROGRAM_GRAPH_NODE_FUNCTION && !effect_function_return_contract_ok(graph, node, path, diag)) return false;
    if (node->kind == Z_PROGRAM_GRAPH_NODE_RETURN && !effect_return_contract_ok(graph, resolution, node, path, diag)) return false;
    if (node->kind == Z_PROGRAM_GRAPH_NODE_CALL || node->kind == Z_PROGRAM_GRAPH_NODE_METHOD_CALL) {
      const ZProgramGraphResolutionReference *ref = NULL;
      if (effect_call_fallible(graph, resolution, node, &ref) && !effect_call_checked(graph, node)) {
        return fail_unchecked_fallible_call(node, ref, path, diag);
      }
    }
  }
  return true;
}
