#include "program_graph_resolve.h"

#include "std_sig.h"
#include "std_source.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define Z_GRAPH_SCOPE_NONE ((size_t)-1)

typedef struct {
  char *id;
  const char *kind;
  size_t node_index;
  size_t parent;
} ZGraphScopeFact;

typedef struct {
  char *name;
  char *kind;
  char *symbol_id;
  char *target_module;
  char *target_name;
  size_t node_index;
  size_t scope_index;
  bool ordered;
  bool is_public;
} ZGraphBindingFact;

typedef struct {
  char *node_id;
  char *kind;
  char *name;
  char *qualified_name;
  char *scope_id;
  char *target_kind;
  char *target_node;
  char *symbol_id;
  char *via_import;
  char *diagnostic_code;
  char *message;
  bool resolved;
  bool ambiguous;
} ZGraphReferenceFact;

typedef struct {
  char *code;
  char *message;
  char *node_id;
  char *name;
} ZGraphDiagnosticFact;

typedef struct {
  const ZProgramGraph *graph;
  ZGraphScopeFact *scopes;
  size_t scope_len, scope_cap;
  ZGraphBindingFact *bindings;
  size_t binding_len, binding_cap;
  ZGraphReferenceFact *references;
  size_t reference_len, reference_cap;
  ZGraphDiagnosticFact *diagnostics;
  size_t diagnostic_len, diagnostic_cap;
} ZGraphResolver;

typedef struct {
  const ZGraphBindingFact *items[16];
  size_t len;
  bool overflow;
} ZGraphLookup;

static bool graph_resolve_text_eq(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}

static bool graph_resolve_text_present(const char *text) {
  return text && text[0];
}

static void graph_resolve_append_quoted(ZBuf *buf, const char *text) {
  zbuf_append_char(buf, '"');
  for (const char *p = text ? text : ""; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    switch (ch) {
      case '\\': zbuf_append(buf, "\\\\"); break;
      case '"': zbuf_append(buf, "\\\""); break;
      case '\n': zbuf_append(buf, "\\n"); break;
      case '\r': zbuf_append(buf, "\\r"); break;
      case '\t': zbuf_append(buf, "\\t"); break;
      default:
        if (ch < 0x20) {
          const char *hex = "0123456789abcdef";
          char escape[7] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 0x0f], 0};
          zbuf_append(buf, escape);
        } else {
          zbuf_append_char(buf, (char)ch);
        }
        break;
    }
  }
  zbuf_append_char(buf, '"');
}

static size_t graph_resolve_node_index(const ZProgramGraph *graph, const char *id) {
  for (size_t i = 0; graph && id && i < graph->node_len; i++) {
    if (graph_resolve_text_eq(graph->nodes[i].id, id)) return i;
  }
  return SIZE_MAX;
}

static const ZProgramGraphNode *graph_resolve_node(const ZProgramGraph *graph, size_t index) {
  return graph && index < graph->node_len ? &graph->nodes[index] : NULL;
}

static const ZProgramGraphEdge *graph_resolve_owner_edge(const ZProgramGraph *graph, const char *node_id) {
  for (size_t i = 0; graph && node_id && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE && graph_resolve_text_eq(edge->to, node_id)) return edge;
  }
  return NULL;
}

static const ZProgramGraphNode *graph_resolve_child(const ZProgramGraph *graph, const ZProgramGraphNode *node, const char *kind, size_t order) {
  for (size_t i = 0; graph && node && kind && i < graph->edge_len; i++) {
    const ZProgramGraphEdge *edge = &graph->edges[i];
    if (edge->target == Z_PROGRAM_GRAPH_EDGE_TARGET_NODE &&
        edge->order == order &&
        graph_resolve_text_eq(edge->from, node->id) &&
        graph_resolve_text_eq(edge->kind, kind)) {
      size_t index = graph_resolve_node_index(graph, edge->to);
      return graph_resolve_node(graph, index);
    }
  }
  return NULL;
}

static bool graph_resolve_node_is_ancestor(const ZProgramGraph *graph, size_t ancestor_index, size_t node_index) {
  const ZProgramGraphNode *ancestor = graph_resolve_node(graph, ancestor_index);
  const ZProgramGraphNode *node = graph_resolve_node(graph, node_index);
  const char *current = node ? node->id : NULL;
  for (size_t depth = 0; graph && ancestor && current && depth < graph->node_len; depth++) {
    if (graph_resolve_text_eq(current, ancestor->id)) return true;
    const ZProgramGraphEdge *owner = graph_resolve_owner_edge(graph, current);
    current = owner ? owner->from : NULL;
  }
  return false;
}

static const char *graph_resolve_scope_kind(const ZProgramGraphNode *node) {
  if (!node) return NULL;
  switch (node->kind) {
    case Z_PROGRAM_GRAPH_NODE_MODULE: return "module";
    case Z_PROGRAM_GRAPH_NODE_IMPORT: return "importAlias";
    case Z_PROGRAM_GRAPH_NODE_C_IMPORT: return "cImportAlias";
    case Z_PROGRAM_GRAPH_NODE_SHAPE:
    case Z_PROGRAM_GRAPH_NODE_INTERFACE:
    case Z_PROGRAM_GRAPH_NODE_ENUM:
    case Z_PROGRAM_GRAPH_NODE_CHOICE:
      return "type";
    case Z_PROGRAM_GRAPH_NODE_FUNCTION: return "function";
    case Z_PROGRAM_GRAPH_NODE_BLOCK: return "block";
    case Z_PROGRAM_GRAPH_NODE_MATCH_ARM: return "match";
    default: return NULL;
  }
}

static char *graph_resolve_scope_id(const ZProgramGraphNode *node) {
  ZBuf out;
  zbuf_init(&out);
  zbuf_append(&out, "scope:");
  zbuf_append(&out, node && node->id ? node->id : "#missing");
  return out.data ? out.data : z_strdup("scope:#missing");
}

static size_t graph_resolve_scope_for_node(const ZGraphResolver *resolver, size_t node_index) {
  for (size_t i = 0; resolver && i < resolver->scope_len; i++) {
    if (resolver->scopes[i].node_index == node_index) return i;
  }
  return Z_GRAPH_SCOPE_NONE;
}

static size_t graph_resolve_nearest_scope(const ZGraphResolver *resolver, size_t node_index) {
  const ZProgramGraph *graph = resolver ? resolver->graph : NULL;
  const ZProgramGraphNode *node = graph_resolve_node(graph, node_index);
  const char *current = node ? node->id : NULL;
  for (size_t depth = 0; graph && current && depth < graph->node_len; depth++) {
    size_t index = graph_resolve_node_index(graph, current);
    size_t scope_index = graph_resolve_scope_for_node(resolver, index);
    if (scope_index != Z_GRAPH_SCOPE_NONE) return scope_index;
    const ZProgramGraphEdge *owner = graph_resolve_owner_edge(graph, current);
    current = owner ? owner->from : NULL;
  }
  return Z_GRAPH_SCOPE_NONE;
}

static size_t graph_resolve_owner_scope(const ZGraphResolver *resolver, size_t node_index) {
  const ZProgramGraph *graph = resolver ? resolver->graph : NULL;
  const ZProgramGraphNode *node = graph_resolve_node(graph, node_index);
  const ZProgramGraphEdge *owner = graph_resolve_owner_edge(graph, node ? node->id : NULL);
  if (!owner) return Z_GRAPH_SCOPE_NONE;
  size_t owner_index = graph_resolve_node_index(graph, owner->from);
  return graph_resolve_nearest_scope(resolver, owner_index);
}

static void graph_resolve_add_scope(ZGraphResolver *resolver, size_t node_index, const char *kind) {
  if (!resolver || !kind) return;
  if (resolver->scope_len == resolver->scope_cap) {
    size_t next = z_grow_capacity(resolver->scope_cap, resolver->scope_len + 1, 32);
    resolver->scopes = z_checked_reallocarray(resolver->scopes, next, sizeof(ZGraphScopeFact));
    for (size_t i = resolver->scope_cap; i < next; i++) resolver->scopes[i] = (ZGraphScopeFact){0};
    resolver->scope_cap = next;
  }
  const ZProgramGraphNode *node = graph_resolve_node(resolver->graph, node_index);
  ZGraphScopeFact *scope = &resolver->scopes[resolver->scope_len++];
  scope->id = graph_resolve_scope_id(node);
  scope->kind = kind;
  scope->node_index = node_index;
  scope->parent = Z_GRAPH_SCOPE_NONE;
}

static const char *graph_resolve_import_binding_name(const ZProgramGraphNode *node) {
  if (!node) return "";
  if (graph_resolve_text_present(node->value)) return node->value;
  const char *module = node->name ? node->name : "";
  const char *last_dot = strrchr(module, '.');
  return last_dot && last_dot[1] ? last_dot + 1 : module;
}

static char *graph_resolve_derived_symbol_id(const ZProgramGraph *graph, const ZProgramGraphNode *node, const char *name, const char *kind) {
  if (node && graph_resolve_text_present(node->symbol_id)) return z_strdup(node->symbol_id);
  const ZProgramGraphNode *module = node;
  for (size_t depth = 0; graph && module && module->kind != Z_PROGRAM_GRAPH_NODE_MODULE && depth < graph->node_len; depth++) {
    const ZProgramGraphEdge *owner = graph_resolve_owner_edge(graph, module->id);
    module = owner ? graph_resolve_node(graph, graph_resolve_node_index(graph, owner->from)) : NULL;
  }
  ZBuf out;
  zbuf_init(&out);
  zbuf_append(&out, "symbol:");
  if (module && graph_resolve_text_present(module->name)) zbuf_append(&out, module->name);
  else if (graph && graph->module_identity && strncmp(graph->module_identity, "module:", strlen("module:")) == 0) zbuf_append(&out, graph->module_identity + strlen("module:"));
  else zbuf_append(&out, graph && graph->module_identity ? graph->module_identity : "main");
  zbuf_append(&out, "::");
  zbuf_append(&out, kind && kind[0] ? kind : "binding");
  zbuf_append_char(&out, '.');
  const char *text = name && name[0] ? name : (node && node->id ? node->id : "anonymous");
  for (const char *p = text; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    zbuf_append_char(&out, (isalnum(ch) || ch == '_' || ch == '-' || ch == '.') ? (char)ch : '_');
  }
  return out.data ? out.data : z_strdup("symbol:module:main::binding.anonymous");
}

static const char *graph_resolve_binding_kind(const ZProgramGraph *graph, const ZProgramGraphNode *node) {
  if (!node) return NULL;
  switch (node->kind) {
    case Z_PROGRAM_GRAPH_NODE_IMPORT: return "import";
    case Z_PROGRAM_GRAPH_NODE_C_IMPORT: return "cImport";
    case Z_PROGRAM_GRAPH_NODE_CONST: return "const";
    case Z_PROGRAM_GRAPH_NODE_TYPE_ALIAS: return "typeAlias";
    case Z_PROGRAM_GRAPH_NODE_SHAPE: return "shape";
    case Z_PROGRAM_GRAPH_NODE_INTERFACE: return "interface";
    case Z_PROGRAM_GRAPH_NODE_ENUM: return "enum";
    case Z_PROGRAM_GRAPH_NODE_CHOICE: return "choice";
    case Z_PROGRAM_GRAPH_NODE_FUNCTION: {
      const ZProgramGraphEdge *owner = graph_resolve_owner_edge(graph, node->id);
      return owner && graph_resolve_text_eq(owner->kind, "method") ? "method" : "function";
    }
    case Z_PROGRAM_GRAPH_NODE_PARAM: {
      const ZProgramGraphEdge *owner = graph_resolve_owner_edge(graph, node->id);
      return owner && graph_resolve_text_eq(owner->kind, "typeParam") ? "typeParam" : "param";
    }
    case Z_PROGRAM_GRAPH_NODE_FIELD: return "field";
    case Z_PROGRAM_GRAPH_NODE_ENUM_CASE:
    case Z_PROGRAM_GRAPH_NODE_CHOICE_CASE: return "variant";
    case Z_PROGRAM_GRAPH_NODE_LET: return "local";
    case Z_PROGRAM_GRAPH_NODE_ERROR_VARIANT: return "error";
    default: return NULL;
  }
}

static char *graph_resolve_binding_name(const ZProgramGraphNode *node) {
  if (!node) return NULL;
  if (node->kind == Z_PROGRAM_GRAPH_NODE_IMPORT) return z_strdup(graph_resolve_import_binding_name(node));
  return graph_resolve_text_present(node->name) ? z_strdup(node->name) : NULL;
}

static bool graph_resolve_binding_is_ordered(const ZProgramGraphNode *node) {
  return node && (node->kind == Z_PROGRAM_GRAPH_NODE_LET || node->kind == Z_PROGRAM_GRAPH_NODE_PARAM);
}

static void graph_resolve_add_binding(ZGraphResolver *resolver, size_t scope_index, size_t node_index, const char *name, const char *kind, const char *target_module, const char *target_name, bool ordered) {
  if (!resolver || scope_index == Z_GRAPH_SCOPE_NONE || !name || !name[0] || !kind) return;
  if (resolver->binding_len == resolver->binding_cap) {
    size_t next = z_grow_capacity(resolver->binding_cap, resolver->binding_len + 1, 64);
    resolver->bindings = z_checked_reallocarray(resolver->bindings, next, sizeof(ZGraphBindingFact));
    for (size_t i = resolver->binding_cap; i < next; i++) resolver->bindings[i] = (ZGraphBindingFact){0};
    resolver->binding_cap = next;
  }
  const ZProgramGraphNode *node = graph_resolve_node(resolver->graph, node_index);
  ZGraphBindingFact *binding = &resolver->bindings[resolver->binding_len++];
  binding->name = z_strdup(name);
  binding->kind = z_strdup(kind);
  binding->symbol_id = graph_resolve_derived_symbol_id(resolver->graph, node, name, kind);
  binding->target_module = graph_resolve_text_present(target_module) ? z_strdup(target_module) : NULL;
  binding->target_name = graph_resolve_text_present(target_name) ? z_strdup(target_name) : NULL;
  binding->node_index = node_index;
  binding->scope_index = scope_index;
  binding->ordered = ordered;
  binding->is_public = node && node->is_public;
}

static void graph_resolve_add_diag(ZGraphResolver *resolver, const char *code, const char *message, const char *node_id, const char *name) {
  if (!resolver || !code || !message) return;
  if (resolver->diagnostic_len == resolver->diagnostic_cap) {
    size_t next = z_grow_capacity(resolver->diagnostic_cap, resolver->diagnostic_len + 1, 16);
    resolver->diagnostics = z_checked_reallocarray(resolver->diagnostics, next, sizeof(ZGraphDiagnosticFact));
    for (size_t i = resolver->diagnostic_cap; i < next; i++) resolver->diagnostics[i] = (ZGraphDiagnosticFact){0};
    resolver->diagnostic_cap = next;
  }
  ZGraphDiagnosticFact *diag = &resolver->diagnostics[resolver->diagnostic_len++];
  diag->code = z_strdup(code);
  diag->message = z_strdup(message);
  diag->node_id = z_strdup(node_id ? node_id : "");
  diag->name = z_strdup(name ? name : "");
}

static ZGraphReferenceFact *graph_resolve_add_reference(ZGraphResolver *resolver, const ZProgramGraphNode *node, const char *kind, const char *name, const char *qualified_name, size_t scope_index) {
  if (!resolver || !node || !kind || !name || !name[0]) return NULL;
  if (resolver->reference_len == resolver->reference_cap) {
    size_t next = z_grow_capacity(resolver->reference_cap, resolver->reference_len + 1, 128);
    resolver->references = z_checked_reallocarray(resolver->references, next, sizeof(ZGraphReferenceFact));
    for (size_t i = resolver->reference_cap; i < next; i++) resolver->references[i] = (ZGraphReferenceFact){0};
    resolver->reference_cap = next;
  }
  ZGraphReferenceFact *ref = &resolver->references[resolver->reference_len++];
  ref->node_id = z_strdup(node->id ? node->id : "");
  ref->kind = z_strdup(kind);
  ref->name = z_strdup(name);
  ref->qualified_name = z_strdup(qualified_name && qualified_name[0] ? qualified_name : name);
  if (scope_index != Z_GRAPH_SCOPE_NONE && scope_index < resolver->scope_len) ref->scope_id = z_strdup(resolver->scopes[scope_index].id);
  return ref;
}

static bool graph_resolve_binding_visible(const ZGraphResolver *resolver, const ZGraphBindingFact *binding, size_t ref_node_index) {
  if (!resolver || !binding) return false;
  if (!binding->ordered) return true;
  const ZProgramGraphNode *binding_node = graph_resolve_node(resolver->graph, binding->node_index);
  const ZProgramGraphNode *ref_node = graph_resolve_node(resolver->graph, ref_node_index);
  if (!binding_node || !ref_node) return true;
  if (graph_resolve_node_is_ancestor(resolver->graph, binding->node_index, ref_node_index)) return false;
  if (binding_node->line < ref_node->line) return true;
  if (binding_node->line == ref_node->line && binding_node->column < ref_node->column) return true;
  return false;
}

static bool graph_resolve_binding_matches_mode(const ZGraphBindingFact *binding, bool type_only) {
  if (!binding) return false;
  bool is_type = graph_resolve_text_eq(binding->kind, "typeAlias") ||
                 graph_resolve_text_eq(binding->kind, "shape") ||
                 graph_resolve_text_eq(binding->kind, "interface") ||
                 graph_resolve_text_eq(binding->kind, "enum") ||
                 graph_resolve_text_eq(binding->kind, "choice") ||
                 graph_resolve_text_eq(binding->kind, "typeParam");
  return type_only ? is_type : true;
}

static void graph_resolve_lookup_add(ZGraphLookup *lookup, const ZGraphBindingFact *binding) {
  if (!lookup || !binding) return;
  if (lookup->len < sizeof(lookup->items) / sizeof(lookup->items[0])) {
    lookup->items[lookup->len++] = binding;
  } else {
    lookup->overflow = true;
  }
}

static const ZProgramGraphNode *graph_resolve_scope_node(const ZGraphResolver *resolver, size_t scope_index) {
  if (!resolver || scope_index >= resolver->scope_len) return NULL;
  return graph_resolve_node(resolver->graph, resolver->scopes[scope_index].node_index);
}

static bool graph_resolve_module_is_stdlib(const ZProgramGraphNode *node) {
  return node && node->kind == Z_PROGRAM_GRAPH_NODE_MODULE && node->name && strncmp(node->name, "std.", 4) == 0;
}

static size_t graph_resolve_module_scope_by_name(const ZGraphResolver *resolver, const char *module_name) {
  for (size_t i = 0; resolver && module_name && i < resolver->scope_len; i++) {
    const ZProgramGraphNode *node = graph_resolve_scope_node(resolver, i);
    if (node && node->kind == Z_PROGRAM_GRAPH_NODE_MODULE && graph_resolve_text_eq(node->name, module_name)) return i;
  }
  return Z_GRAPH_SCOPE_NONE;
}

static void graph_resolve_lookup_imports(const ZGraphResolver *resolver, size_t module_scope, const char *name, bool type_only, ZGraphLookup *lookup) {
  if (!resolver || module_scope == Z_GRAPH_SCOPE_NONE || !name || !lookup) return;
  for (size_t i = 0; i < resolver->binding_len; i++) {
    const ZGraphBindingFact *import = &resolver->bindings[i];
    if (import->scope_index != module_scope || !graph_resolve_text_eq(import->kind, "import")) continue;
    size_t target_scope = graph_resolve_module_scope_by_name(resolver, import->target_module);
    if (target_scope == Z_GRAPH_SCOPE_NONE) continue;
    for (size_t j = 0; j < resolver->binding_len; j++) {
      const ZGraphBindingFact *candidate = &resolver->bindings[j];
      if (candidate->scope_index == target_scope &&
          graph_resolve_text_eq(candidate->name, name) &&
          graph_resolve_binding_matches_mode(candidate, type_only)) {
        graph_resolve_lookup_add(lookup, candidate);
      }
    }
  }
}

static void graph_resolve_lookup_package_modules(const ZGraphResolver *resolver, size_t module_scope, const char *name, bool type_only, ZGraphLookup *lookup) {
  if (!resolver || module_scope == Z_GRAPH_SCOPE_NONE || !name || !lookup) return;
  const ZProgramGraphNode *current_module = graph_resolve_scope_node(resolver, module_scope);
  if (!current_module || graph_resolve_module_is_stdlib(current_module)) return;
  for (size_t i = 0; i < resolver->scope_len; i++) {
    const ZProgramGraphNode *module = graph_resolve_scope_node(resolver, i);
    if (!module || module->kind != Z_PROGRAM_GRAPH_NODE_MODULE || i == module_scope || graph_resolve_module_is_stdlib(module)) continue;
    for (size_t j = 0; j < resolver->binding_len; j++) {
      const ZGraphBindingFact *candidate = &resolver->bindings[j];
      if (candidate->scope_index == i &&
          graph_resolve_text_eq(candidate->name, name) &&
          graph_resolve_binding_matches_mode(candidate, type_only)) {
        graph_resolve_lookup_add(lookup, candidate);
      }
    }
  }
}

static ZGraphLookup graph_resolve_lookup_name(const ZGraphResolver *resolver, size_t scope_index, const char *name, size_t ref_node_index, bool type_only) {
  ZGraphLookup lookup = {0};
  for (size_t current = scope_index; resolver && current != Z_GRAPH_SCOPE_NONE && current < resolver->scope_len; current = resolver->scopes[current].parent) {
    for (size_t i = 0; i < resolver->binding_len; i++) {
      const ZGraphBindingFact *binding = &resolver->bindings[i];
      if (binding->scope_index == current &&
          graph_resolve_text_eq(binding->name, name) &&
          graph_resolve_binding_matches_mode(binding, type_only) &&
          graph_resolve_binding_visible(resolver, binding, ref_node_index)) {
        graph_resolve_lookup_add(&lookup, binding);
      }
    }
    if (lookup.len > 0 || lookup.overflow) return lookup;
    const ZProgramGraphNode *scope_node = graph_resolve_scope_node(resolver, current);
    if (scope_node && scope_node->kind == Z_PROGRAM_GRAPH_NODE_MODULE) {
      graph_resolve_lookup_imports(resolver, current, name, type_only, &lookup);
      if (lookup.len > 0 || lookup.overflow) return lookup;
      graph_resolve_lookup_package_modules(resolver, current, name, type_only, &lookup);
      if (lookup.len > 0 || lookup.overflow) return lookup;
    }
  }
  return lookup;
}

static const ZGraphBindingFact *graph_resolve_lookup_member(const ZGraphResolver *resolver, const ZGraphBindingFact *owner, const char *member) {
  if (!resolver || !owner || !member) return NULL;
  if (graph_resolve_text_eq(owner->kind, "import")) {
    size_t module_scope = graph_resolve_module_scope_by_name(resolver, owner->target_module);
    for (size_t i = 0; i < resolver->binding_len; i++) {
      const ZGraphBindingFact *binding = &resolver->bindings[i];
      if (binding->scope_index == module_scope && graph_resolve_text_eq(binding->name, member)) return binding;
    }
  }
  const ZProgramGraphNode *owner_node = graph_resolve_node(resolver->graph, owner->node_index);
  size_t owner_scope = graph_resolve_scope_for_node(resolver, owner->node_index);
  if (owner_node && owner_scope != Z_GRAPH_SCOPE_NONE) {
    for (size_t i = 0; i < resolver->binding_len; i++) {
      const ZGraphBindingFact *binding = &resolver->bindings[i];
      if (binding->scope_index == owner_scope && graph_resolve_text_eq(binding->name, member)) return binding;
    }
  }
  return NULL;
}

static char *graph_resolve_expr_chain(const ZGraphResolver *resolver, const ZProgramGraphNode *node) {
  if (!resolver || !node) return NULL;
  if (node->kind == Z_PROGRAM_GRAPH_NODE_IDENTIFIER) return graph_resolve_text_present(node->name) ? z_strdup(node->name) : NULL;
  if (node->kind == Z_PROGRAM_GRAPH_NODE_FIELD_ACCESS) {
    const ZProgramGraphNode *left = graph_resolve_child(resolver->graph, node, "left", 0);
    char *left_name = graph_resolve_expr_chain(resolver, left);
    if (!left_name || !left_name[0] || !graph_resolve_text_present(node->name)) {
      free(left_name);
      return NULL;
    }
    ZBuf out;
    zbuf_init(&out);
    zbuf_append(&out, left_name);
    zbuf_append_char(&out, '.');
    zbuf_append(&out, node->name);
    free(left_name);
    return out.data ? out.data : NULL;
  }
  if (node->kind == Z_PROGRAM_GRAPH_NODE_METHOD_CALL || node->kind == Z_PROGRAM_GRAPH_NODE_CALL) {
    const ZProgramGraphNode *left = graph_resolve_child(resolver->graph, node, "left", 0);
    char *left_name = graph_resolve_expr_chain(resolver, left);
    if (left_name) return left_name;
    return graph_resolve_text_present(node->name) ? z_strdup(node->name) : NULL;
  }
  return NULL;
}

static bool graph_resolve_name_operator(const char *name) {
  if (!name || !name[0]) return false;
  for (const char *p = name; *p; p++) {
    if (isalnum((unsigned char)*p) || *p == '_' || *p == '.') return false;
  }
  return true;
}

static char *graph_resolve_first_segment(const char *name) {
  const char *dot = name ? strchr(name, '.') : NULL;
  return dot ? z_strndup(name, (size_t)(dot - name)) : (name ? z_strdup(name) : NULL);
}

static const char *graph_resolve_last_segment(const char *name) {
  const char *dot = name ? strrchr(name, '.') : NULL;
  return dot && dot[1] ? dot + 1 : (name ? name : "");
}

static bool graph_resolve_builtin_type(const char *name) {
  static const char *const builtins[] = {
    "Void", "Bool", "bool", "String", "char", "Type",
    "World", "WorldStream", "Fs", "File", "ByteBuf", "NullAlloc", "FixedBufAlloc", "PageAlloc", "GeneralAlloc",
    "Vec", "Duration", "RandSource", "ProcStatus", "Address", "Net", "Conn", "Listener",
    "HttpMethod", "HttpClient", "HttpServer", "HttpResult", "HttpError", "HttpHeaderValue", "JsonDoc", "BufferedReader", "BufferedWriter",
    "Env", "Args", "Clock", "Rand", "Proc", "Alloc",
    "Maybe", "Span", "MutSpan",
    "ref", "mutref", "owned", "usize", "isize", "u8", "u16", "u32", "u64",
    "i8", "i16", "i32", "i64", "f32", "f64", NULL
  };
  if (!name || !name[0] || name[0] == '[') return true;
  for (size_t i = 0; builtins[i]; i++) {
    size_t len = strlen(builtins[i]);
    if (strncmp(name, builtins[i], len) == 0 && (!name[len] || name[len] == '<')) return true;
  }
  return false;
}

static char *graph_resolve_simple_type_name(const char *type) {
  if (!type || !(isalpha((unsigned char)type[0]) || type[0] == '_')) return NULL;
  const char *end = type;
  while (isalnum((unsigned char)*end) || *end == '_' || *end == '.') end++;
  if (*end && *end != '<') return NULL;
  return z_strndup(type, (size_t)(end - type));
}

static const ZGraphBindingFact *graph_resolve_constrained_interface_member(const ZGraphResolver *resolver, const ZGraphBindingFact *owner, const char *member) {
  if (!resolver || !owner || !member || !graph_resolve_text_eq(owner->kind, "typeParam")) return NULL;
  const ZProgramGraphNode *owner_node = graph_resolve_node(resolver->graph, owner->node_index);
  char *interface_name = graph_resolve_simple_type_name(owner_node ? owner_node->type : NULL);
  if (!interface_name || graph_resolve_text_eq(interface_name, "Type")) {
    free(interface_name);
    return NULL;
  }
  ZGraphLookup lookup = graph_resolve_lookup_name(resolver, owner->scope_index, interface_name, owner->node_index, true);
  free(interface_name);
  if (lookup.len != 1 || lookup.overflow || !graph_resolve_text_eq(lookup.items[0]->kind, "interface")) return NULL;
  return graph_resolve_lookup_member(resolver, lookup.items[0], member);
}

static void graph_resolve_reference_to_binding(ZGraphReferenceFact *ref, const ZGraphResolver *resolver, const ZGraphBindingFact *binding, const char *target_kind, const char *via_import) {
  if (!ref || !binding) return;
  const ZProgramGraphNode *target = graph_resolve_node(resolver ? resolver->graph : NULL, binding->node_index);
  ref->resolved = true;
  ref->target_kind = z_strdup(target_kind && target_kind[0] ? target_kind : binding->kind);
  ref->target_node = z_strdup(target && target->id ? target->id : "");
  ref->symbol_id = z_strdup(binding->symbol_id ? binding->symbol_id : "");
  if (via_import && via_import[0]) ref->via_import = z_strdup(via_import);
}

static size_t graph_resolve_enclosing_module_scope(const ZGraphResolver *resolver, size_t scope_index) {
  for (size_t current = scope_index; resolver && current != Z_GRAPH_SCOPE_NONE && current < resolver->scope_len; current = resolver->scopes[current].parent) {
    const ZProgramGraphNode *node = graph_resolve_scope_node(resolver, current);
    if (node && node->kind == Z_PROGRAM_GRAPH_NODE_MODULE) return current;
  }
  return Z_GRAPH_SCOPE_NONE;
}

static const char *graph_resolve_binding_module_name(const ZGraphResolver *resolver, const ZGraphBindingFact *binding) {
  if (!resolver || !binding || binding->scope_index >= resolver->scope_len) return NULL;
  size_t module_scope = graph_resolve_enclosing_module_scope(resolver, binding->scope_index);
  const ZProgramGraphNode *module = graph_resolve_scope_node(resolver, module_scope);
  return module ? module->name : NULL;
}

static const ZGraphBindingFact *graph_resolve_import_for_binding(const ZGraphResolver *resolver, size_t ref_scope, const ZGraphBindingFact *target) {
  const char *target_module = graph_resolve_binding_module_name(resolver, target);
  size_t ref_module_scope = graph_resolve_enclosing_module_scope(resolver, ref_scope);
  const ZProgramGraphNode *ref_module = graph_resolve_scope_node(resolver, ref_module_scope);
  if (!target_module || !ref_module || graph_resolve_text_eq(target_module, ref_module->name)) return NULL;
  for (size_t i = 0; resolver && i < resolver->binding_len; i++) {
    const ZGraphBindingFact *binding = &resolver->bindings[i];
    if (binding->scope_index == ref_module_scope &&
        graph_resolve_text_eq(binding->kind, "import") &&
        graph_resolve_text_eq(binding->target_module, target_module)) {
      return binding;
    }
  }
  return NULL;
}

static void graph_resolve_reference_builtin(ZGraphReferenceFact *ref, const char *target_kind, const char *symbol_id) {
  if (!ref) return;
  ref->resolved = true;
  ref->target_kind = z_strdup(target_kind ? target_kind : "builtin");
  ref->symbol_id = z_strdup(symbol_id ? symbol_id : "");
}

static void graph_resolve_reference_diag(ZGraphResolver *resolver, ZGraphReferenceFact *ref, const char *code, const char *message) {
  if (!resolver || !ref || !code || !message) return;
  ref->diagnostic_code = z_strdup(code);
  ref->message = z_strdup(message);
  if (!ref->resolved) graph_resolve_add_diag(resolver, code, message, ref->node_id, ref->name);
}

static void graph_resolve_apply_lookup(ZGraphResolver *resolver, ZGraphReferenceFact *ref, const ZGraphLookup *lookup, const char *target_kind) {
  if (!resolver || !ref || !lookup) return;
  if (lookup->len == 1 && !lookup->overflow) {
    const ZGraphBindingFact *via = graph_resolve_import_for_binding(resolver, graph_resolve_nearest_scope(resolver, graph_resolve_node_index(resolver->graph, ref->node_id)), lookup->items[0]);
    graph_resolve_reference_to_binding(ref, resolver, lookup->items[0], target_kind, via ? via->symbol_id : NULL);
    return;
  }
  char message[192];
  if (lookup->len > 1 || lookup->overflow) {
    snprintf(message, sizeof(message), "ambiguous graph reference '%s'", ref->name ? ref->name : "");
    ref->ambiguous = true;
    graph_resolve_reference_diag(resolver, ref, "NAM004", message);
    return;
  }
  snprintf(message, sizeof(message), "unknown identifier '%s'", ref->name ? ref->name : "");
  graph_resolve_reference_diag(resolver, ref, "NAM003", message);
}

static const ZGraphBindingFact *graph_resolve_import_binding_for_module(const ZGraphResolver *resolver, size_t scope_index, const char *module_name) {
  for (size_t current = scope_index; resolver && current != Z_GRAPH_SCOPE_NONE && current < resolver->scope_len; current = resolver->scopes[current].parent) {
    for (size_t i = 0; i < resolver->binding_len; i++) {
      const ZGraphBindingFact *binding = &resolver->bindings[i];
      if (binding->scope_index == current &&
          graph_resolve_text_eq(binding->kind, "import") &&
          graph_resolve_text_eq(binding->target_module, module_name)) {
        return binding;
      }
    }
  }
  return NULL;
}

static const ZGraphBindingFact *graph_resolve_std_source_binding(const ZGraphResolver *resolver, const char *qualified_name) {
  const char *target_name = z_std_source_target_for_public_call(qualified_name);
  const ZStdSourceModule *module = z_std_source_module_for_public_call(qualified_name);
  if (!resolver || !target_name || !module) return NULL;
  size_t scope = graph_resolve_module_scope_by_name(resolver, module->module);
  for (size_t i = 0; i < resolver->binding_len; i++) {
    const ZGraphBindingFact *binding = &resolver->bindings[i];
    if (binding->scope_index == scope && graph_resolve_text_eq(binding->name, target_name)) return binding;
  }
  return NULL;
}

static void graph_resolve_call_reference(ZGraphResolver *resolver, size_t node_index) {
  const ZProgramGraphNode *node = graph_resolve_node(resolver ? resolver->graph : NULL, node_index);
  if (!node || (node->kind != Z_PROGRAM_GRAPH_NODE_CALL && node->kind != Z_PROGRAM_GRAPH_NODE_METHOD_CALL)) return;
  if (graph_resolve_name_operator(node->name)) return;
  char *qualified = graph_resolve_expr_chain(resolver, node);
  if (!qualified || !qualified[0] || graph_resolve_name_operator(qualified)) {
    free(qualified);
    return;
  }
  size_t scope = graph_resolve_nearest_scope(resolver, node_index);
  ZGraphReferenceFact *ref = graph_resolve_add_reference(resolver, node, "call", graph_resolve_last_segment(qualified), qualified, scope);
  if (!ref) {
    free(qualified);
    return;
  }

  char *first = graph_resolve_first_segment(qualified);
  const char *dot = strchr(qualified, '.');
  bool first_is_shadowed = false;
  if (first && dot && dot[1]) {
    ZGraphLookup owner_lookup = graph_resolve_lookup_name(resolver, scope, first, node_index, false);
    first_is_shadowed = owner_lookup.len > 0 || owner_lookup.overflow;
    if (owner_lookup.len == 1 && !owner_lookup.overflow) {
      const ZGraphBindingFact *owner = owner_lookup.items[0];
      if (graph_resolve_text_eq(owner->kind, "cImport")) {
        ref->resolved = true;
        ref->target_kind = z_strdup("cFunction");
        ref->target_node = z_strdup(graph_resolve_node(resolver->graph, owner->node_index)->id);
        ref->symbol_id = z_strdup(owner->symbol_id ? owner->symbol_id : "");
        ref->via_import = z_strdup(owner->symbol_id ? owner->symbol_id : "");
        free(ref->name);
        ref->name = z_strdup(dot + 1);
        free(first);
        free(qualified);
        return;
      }
      const ZGraphBindingFact *constrained_member = graph_resolve_constrained_interface_member(resolver, owner, dot + 1);
      if (constrained_member) {
        graph_resolve_reference_to_binding(ref, resolver, constrained_member, "interfaceMethod", NULL);
        free(first);
        free(qualified);
        return;
      }
      const ZGraphBindingFact *member = graph_resolve_lookup_member(resolver, owner, dot + 1);
      if (member) {
        graph_resolve_reference_to_binding(ref, resolver, member, member->kind, owner->symbol_id);
        free(first);
        free(qualified);
        return;
      }
      if (graph_resolve_text_eq(owner->kind, "param") || graph_resolve_text_eq(owner->kind, "local")) {
        graph_resolve_reference_to_binding(ref, resolver, owner, "member", NULL);
        free(first);
        free(qualified);
        return;
      }
    }
  }

  if (strncmp(qualified, "std.", 4) == 0 && !first_is_shadowed) {
    const ZGraphBindingFact *source_binding = graph_resolve_std_source_binding(resolver, qualified);
    if (source_binding) {
      const ZGraphBindingFact *via = graph_resolve_import_binding_for_module(resolver, scope, z_std_source_module_for_public_call(qualified)->module);
      graph_resolve_reference_to_binding(ref, resolver, source_binding, "sourceBackedStdlib", via ? via->symbol_id : NULL);
    } else if (z_std_helper_find(qualified)) {
      ZBuf symbol;
      zbuf_init(&symbol);
      zbuf_append(&symbol, "stdlib:");
      zbuf_append(&symbol, qualified);
      graph_resolve_reference_builtin(ref, "stdlib", symbol.data ? symbol.data : "");
      zbuf_free(&symbol);
    } else {
      char message[192];
      snprintf(message, sizeof(message), "unknown identifier '%s'", qualified);
      graph_resolve_reference_diag(resolver, ref, "NAM003", message);
    }
    free(first);
    free(qualified);
    return;
  }

  ZGraphLookup lookup = graph_resolve_lookup_name(resolver, scope, qualified, node_index, false);
  if (lookup.len == 0 && strchr(qualified, '.') == NULL) lookup = graph_resolve_lookup_name(resolver, scope, graph_resolve_last_segment(qualified), node_index, false);
  graph_resolve_apply_lookup(resolver, ref, &lookup, NULL);
  free(first);
  free(qualified);
}

static bool graph_resolve_identifier_is_call_callee(const ZGraphResolver *resolver, const ZProgramGraphNode *node) {
  const ZProgramGraphEdge *owner = graph_resolve_owner_edge(resolver ? resolver->graph : NULL, node ? node->id : NULL);
  if (!owner || !graph_resolve_text_eq(owner->kind, "left")) return false;
  size_t owner_index = graph_resolve_node_index(resolver->graph, owner->from);
  const ZProgramGraphNode *owner_node = graph_resolve_node(resolver->graph, owner_index);
  return owner_node && owner_node->kind == Z_PROGRAM_GRAPH_NODE_CALL && graph_resolve_text_eq(owner_node->name, node->name);
}

static void graph_resolve_identifier_reference(ZGraphResolver *resolver, size_t node_index) {
  const ZProgramGraphNode *node = graph_resolve_node(resolver ? resolver->graph : NULL, node_index);
  if (!node || node->kind != Z_PROGRAM_GRAPH_NODE_IDENTIFIER || !graph_resolve_text_present(node->name)) return;
  if (graph_resolve_identifier_is_call_callee(resolver, node)) return;
  size_t scope = graph_resolve_nearest_scope(resolver, node_index);
  ZGraphReferenceFact *ref = graph_resolve_add_reference(resolver, node, "identifier", node->name, node->name, scope);
  if (!ref) return;
  ZGraphLookup lookup = graph_resolve_lookup_name(resolver, scope, node->name, node_index, false);
  if (lookup.len == 0 && !lookup.overflow && graph_resolve_text_eq(node->name, "std")) {
    graph_resolve_reference_builtin(ref, "stdlibNamespace", "stdlib:std");
    return;
  }
  graph_resolve_apply_lookup(resolver, ref, &lookup, NULL);
}

static void graph_resolve_type_reference(ZGraphResolver *resolver, size_t node_index) {
  const ZProgramGraphNode *node = graph_resolve_node(resolver ? resolver->graph : NULL, node_index);
  if (!node || node->kind != Z_PROGRAM_GRAPH_NODE_TYPE_REF || !graph_resolve_text_present(node->type)) return;
  char *name = graph_resolve_simple_type_name(node->type);
  if (!name) return;
  size_t scope = graph_resolve_nearest_scope(resolver, node_index);
  ZGraphReferenceFact *ref = graph_resolve_add_reference(resolver, node, "type", name, node->type, scope);
  if (!ref) {
    free(name);
    return;
  }
  if (graph_resolve_builtin_type(name)) {
    ZBuf symbol;
    zbuf_init(&symbol);
    zbuf_append(&symbol, "builtin:");
    zbuf_append(&symbol, name);
    graph_resolve_reference_builtin(ref, "builtinType", symbol.data ? symbol.data : "");
    zbuf_free(&symbol);
    free(name);
    return;
  }
  ZGraphLookup lookup = graph_resolve_lookup_name(resolver, scope, name, node_index, true);
  graph_resolve_apply_lookup(resolver, ref, &lookup, "type");
  free(name);
}

static void graph_resolve_build_scopes(ZGraphResolver *resolver) {
  for (size_t i = 0; resolver && i < resolver->graph->node_len; i++) {
    const char *kind = graph_resolve_scope_kind(&resolver->graph->nodes[i]);
    if (kind) graph_resolve_add_scope(resolver, i, kind);
  }
  for (size_t i = 0; resolver && i < resolver->scope_len; i++) {
    const ZProgramGraphNode *node = graph_resolve_scope_node(resolver, i);
    if (!node || node->kind == Z_PROGRAM_GRAPH_NODE_MODULE) continue;
    resolver->scopes[i].parent = graph_resolve_owner_scope(resolver, resolver->scopes[i].node_index);
  }
}

static void graph_resolve_build_bindings(ZGraphResolver *resolver) {
  for (size_t i = 0; resolver && i < resolver->graph->node_len; i++) {
    const ZProgramGraphNode *node = &resolver->graph->nodes[i];
    const char *kind = graph_resolve_binding_kind(resolver->graph, node);
    if (!kind) continue;
    char *name = graph_resolve_binding_name(node);
    size_t owner_scope = graph_resolve_owner_scope(resolver, i);
    if (node->kind == Z_PROGRAM_GRAPH_NODE_PARAM) {
      const ZProgramGraphEdge *owner = graph_resolve_owner_edge(resolver->graph, node->id);
      if (owner) owner_scope = graph_resolve_scope_for_node(resolver, graph_resolve_node_index(resolver->graph, owner->from));
    } else if (node->kind == Z_PROGRAM_GRAPH_NODE_LET) {
      owner_scope = graph_resolve_owner_scope(resolver, i);
    }
    graph_resolve_add_binding(resolver,
                              owner_scope,
                              i,
                              name,
                              kind,
                              node->kind == Z_PROGRAM_GRAPH_NODE_IMPORT ? node->name : NULL,
                              NULL,
                              graph_resolve_binding_is_ordered(node));
    free(name);
  }
  for (size_t i = 0; resolver && i < resolver->graph->node_len; i++) {
    const ZProgramGraphNode *node = &resolver->graph->nodes[i];
    if (node->kind != Z_PROGRAM_GRAPH_NODE_MATCH_ARM || !graph_resolve_text_present(node->value)) continue;
    size_t scope = graph_resolve_scope_for_node(resolver, i);
    graph_resolve_add_binding(resolver, scope, i, node->value, "pattern", NULL, NULL, false);
  }
}

static void graph_resolve_build_references(ZGraphResolver *resolver) {
  for (size_t i = 0; resolver && i < resolver->graph->node_len; i++) {
    const ZProgramGraphNode *node = &resolver->graph->nodes[i];
    if (node->kind == Z_PROGRAM_GRAPH_NODE_IDENTIFIER) graph_resolve_identifier_reference(resolver, i);
    else if (node->kind == Z_PROGRAM_GRAPH_NODE_CALL || node->kind == Z_PROGRAM_GRAPH_NODE_METHOD_CALL) graph_resolve_call_reference(resolver, i);
    else if (node->kind == Z_PROGRAM_GRAPH_NODE_TYPE_REF) graph_resolve_type_reference(resolver, i);
  }
}

static void graph_resolve_free(ZGraphResolver *resolver) {
  if (!resolver) return;
  for (size_t i = 0; i < resolver->scope_len; i++) free(resolver->scopes[i].id);
  for (size_t i = 0; i < resolver->binding_len; i++) {
    free(resolver->bindings[i].name);
    free(resolver->bindings[i].kind);
    free(resolver->bindings[i].symbol_id);
    free(resolver->bindings[i].target_module);
    free(resolver->bindings[i].target_name);
  }
  for (size_t i = 0; i < resolver->reference_len; i++) {
    free(resolver->references[i].node_id);
    free(resolver->references[i].kind);
    free(resolver->references[i].name);
    free(resolver->references[i].qualified_name);
    free(resolver->references[i].scope_id);
    free(resolver->references[i].target_kind);
    free(resolver->references[i].target_node);
    free(resolver->references[i].symbol_id);
    free(resolver->references[i].via_import);
    free(resolver->references[i].diagnostic_code);
    free(resolver->references[i].message);
  }
  for (size_t i = 0; i < resolver->diagnostic_len; i++) {
    free(resolver->diagnostics[i].code);
    free(resolver->diagnostics[i].message);
    free(resolver->diagnostics[i].node_id);
    free(resolver->diagnostics[i].name);
  }
  free(resolver->scopes);
  free(resolver->bindings);
  free(resolver->references);
  free(resolver->diagnostics);
  *resolver = (ZGraphResolver){0};
}

static void graph_resolve_append_binding_json(ZBuf *buf, const ZGraphResolver *resolver, const ZGraphBindingFact *binding) {
  const ZProgramGraphNode *node = graph_resolve_node(resolver ? resolver->graph : NULL, binding ? binding->node_index : SIZE_MAX);
  zbuf_append(buf, "{\"name\":");
  graph_resolve_append_quoted(buf, binding ? binding->name : "");
  zbuf_append(buf, ",\"kind\":");
  graph_resolve_append_quoted(buf, binding ? binding->kind : "");
  zbuf_append(buf, ",\"node\":");
  graph_resolve_append_quoted(buf, node ? node->id : "");
  zbuf_append(buf, ",\"symbolId\":");
  graph_resolve_append_quoted(buf, binding ? binding->symbol_id : "");
  zbuf_appendf(buf, ",\"public\":%s", binding && binding->is_public ? "true" : "false");
  if (binding && binding->target_module) {
    zbuf_append(buf, ",\"targetModule\":");
    graph_resolve_append_quoted(buf, binding->target_module);
  }
  zbuf_append(buf, "}");
}

static void graph_resolve_append_scopes_json(ZBuf *buf, const ZGraphResolver *resolver) {
  zbuf_append(buf, "[");
  for (size_t i = 0; resolver && i < resolver->scope_len; i++) {
    const ZGraphScopeFact *scope = &resolver->scopes[i];
    const ZProgramGraphNode *node = graph_resolve_scope_node(resolver, i);
    if (i > 0) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"id\":");
    graph_resolve_append_quoted(buf, scope->id);
    zbuf_append(buf, ",\"kind\":");
    graph_resolve_append_quoted(buf, scope->kind);
    zbuf_append(buf, ",\"node\":");
    graph_resolve_append_quoted(buf, node ? node->id : "");
    zbuf_append(buf, ",\"name\":");
    graph_resolve_append_quoted(buf, node && node->name ? node->name : "");
    zbuf_append(buf, ",\"parent\":");
    if (scope->parent != Z_GRAPH_SCOPE_NONE && scope->parent < resolver->scope_len) graph_resolve_append_quoted(buf, resolver->scopes[scope->parent].id);
    else zbuf_append(buf, "null");
    zbuf_append(buf, ",\"bindings\":[");
    bool first = true;
    for (size_t j = 0; j < resolver->binding_len; j++) {
      if (resolver->bindings[j].scope_index != i) continue;
      if (!first) zbuf_append(buf, ",");
      graph_resolve_append_binding_json(buf, resolver, &resolver->bindings[j]);
      first = false;
    }
    zbuf_append(buf, "]}");
  }
  zbuf_append(buf, "]");
}

static void graph_resolve_append_references_json(ZBuf *buf, const ZGraphResolver *resolver) {
  zbuf_append(buf, "[");
  for (size_t i = 0; resolver && i < resolver->reference_len; i++) {
    const ZGraphReferenceFact *ref = &resolver->references[i];
    if (i > 0) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"node\":");
    graph_resolve_append_quoted(buf, ref->node_id);
    zbuf_append(buf, ",\"kind\":");
    graph_resolve_append_quoted(buf, ref->kind);
    zbuf_append(buf, ",\"name\":");
    graph_resolve_append_quoted(buf, ref->name);
    zbuf_append(buf, ",\"qualifiedName\":");
    graph_resolve_append_quoted(buf, ref->qualified_name);
    zbuf_append(buf, ",\"scope\":");
    graph_resolve_append_quoted(buf, ref->scope_id);
    zbuf_appendf(buf, ",\"resolved\":%s,\"ambiguous\":%s", ref->resolved ? "true" : "false", ref->ambiguous ? "true" : "false");
    zbuf_append(buf, ",\"targetKind\":");
    graph_resolve_append_quoted(buf, ref->target_kind);
    zbuf_append(buf, ",\"targetNode\":");
    graph_resolve_append_quoted(buf, ref->target_node);
    zbuf_append(buf, ",\"symbolId\":");
    graph_resolve_append_quoted(buf, ref->symbol_id);
    zbuf_append(buf, ",\"viaImport\":");
    graph_resolve_append_quoted(buf, ref->via_import);
    zbuf_append(buf, ",\"diagnostic\":");
    if (ref->diagnostic_code) {
      zbuf_append(buf, "{\"code\":");
      graph_resolve_append_quoted(buf, ref->diagnostic_code);
      zbuf_append(buf, ",\"message\":");
      graph_resolve_append_quoted(buf, ref->message);
      zbuf_append(buf, "}");
    } else {
      zbuf_append(buf, "null");
    }
    zbuf_append(buf, "}");
  }
  zbuf_append(buf, "]");
}

static void graph_resolve_append_diagnostics_json(ZBuf *buf, const ZGraphResolver *resolver) {
  zbuf_append(buf, "[");
  for (size_t i = 0; resolver && i < resolver->diagnostic_len; i++) {
    const ZGraphDiagnosticFact *diag = &resolver->diagnostics[i];
    if (i > 0) zbuf_append(buf, ",");
    zbuf_append(buf, "{\"code\":");
    graph_resolve_append_quoted(buf, diag->code);
    zbuf_append(buf, ",\"message\":");
    graph_resolve_append_quoted(buf, diag->message);
    zbuf_append(buf, ",\"node\":");
    graph_resolve_append_quoted(buf, diag->node_id);
    zbuf_append(buf, ",\"name\":");
    graph_resolve_append_quoted(buf, diag->name);
    zbuf_append(buf, "}");
  }
  zbuf_append(buf, "]");
}

void z_program_graph_append_resolution_json(ZBuf *buf, const ZProgramGraph *graph) {
  ZGraphResolver resolver = {.graph = graph};
  graph_resolve_build_scopes(&resolver);
  graph_resolve_build_bindings(&resolver);
  graph_resolve_build_references(&resolver);
  zbuf_append(buf, "{\"state\":\"resolved\",\"ok\":");
  zbuf_append(buf, resolver.diagnostic_len == 0 ? "true" : "false");
  zbuf_appendf(buf, ",\"counts\":{\"scopes\":%zu,\"bindings\":%zu,\"references\":%zu,\"diagnostics\":%zu}", resolver.scope_len, resolver.binding_len, resolver.reference_len, resolver.diagnostic_len);
  zbuf_append(buf, ",\"scopes\":");
  graph_resolve_append_scopes_json(buf, &resolver);
  zbuf_append(buf, ",\"references\":");
  graph_resolve_append_references_json(buf, &resolver);
  zbuf_append(buf, ",\"diagnostics\":");
  graph_resolve_append_diagnostics_json(buf, &resolver);
  zbuf_append(buf, "}");
  graph_resolve_free(&resolver);
}
