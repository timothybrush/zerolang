#include "program_graph_build.h"
#include "program_graph_import.h"
#include "program_graph_lower.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool graph_compile_text_eq(const char *left, const char *right) { return strcmp(left ? left : "", right ? right : "") == 0; }

bool z_program_graph_source_command_uses_graph_mir(const char *command) {
  if (!command || graph_compile_text_eq(command, "fix") || graph_compile_text_eq(command, "doc")) return false;
  if (graph_compile_text_eq(command, "dev") || graph_compile_text_eq(command, "time") || graph_compile_text_eq(command, "abi")) return false;
  return true;
}

static bool graph_compile_diag(ZDiag *diag, const char *path, const char *message, const char *actual) {
  if (!diag) return false;
  *diag = (ZDiag){0};
  diag->code = 2002; diag->path = path; diag->line = 1; diag->column = 1; diag->length = 1;
  snprintf(diag->message, sizeof(diag->message), "%s", message ? message : "failed to prepare source program graph");
  snprintf(diag->expected, sizeof(diag->expected), "source-imported ProgramGraph"); snprintf(diag->actual, sizeof(diag->actual), "%s", actual ? actual : "invalid graph");
  snprintf(diag->help, sizeof(diag->help), "run zero graph check to inspect the source graph");
  return false;
}

static void graph_compile_pin_diag_paths(ZDiag *diag) {
  if (!diag) return;
  if (diag->path) diag->path = z_strdup(diag->path);
  for (size_t i = 0; i < diag->borrow_trace_count; i++) {
    if (diag->borrow_traces[i].binding_decl_path) {
      diag->borrow_traces[i].binding_decl_path = z_strdup(diag->borrow_traces[i].binding_decl_path);
    }
  }
}

static void graph_compile_replace_source_map(SourceInput *input, SourceInput *graph_input) {
  if (!input || !graph_input) return;
  for (size_t i = 0; i < input->source_line_count; i++) free(input->source_line_paths[i]);
  free(input->source_line_paths); free(input->source_line_numbers);
  input->source_line_paths = graph_input->source_line_paths; input->source_line_numbers = graph_input->source_line_numbers; input->source_line_count = graph_input->source_line_count;
  graph_input->source_line_paths = NULL; graph_input->source_line_numbers = NULL; graph_input->source_line_count = 0;
}

bool z_program_graph_prepare_source_mir_input(const char *source_path, const ZTargetInfo *target, Program *program, SourceInput *input, IrProgram *ir, ZProgramGraphArtifactSource *source, ZDiag *diag) {
  if (!program || !input || !ir) return graph_compile_diag(diag, source_path, "failed to prepare source program graph", "missing compiler input");
  const char *path = input->source_file ? input->source_file : source_path;
  ZProgramGraph graph = {0};
  if (!z_program_graph_from_program(input, program, &graph)) return graph_compile_diag(diag, path, "failed to build source program graph", "source import failed");
  graph.canonical_source = input->canonical_text_source;
  ZProgramGraphValidation validation = {0};
  if (!z_program_graph_validate(&graph, &validation)) {
    const char *actual = validation.message[0] ? validation.message : validation.code;
    z_program_graph_free(&graph);
    return graph_compile_diag(diag, path, "source program graph is not valid", actual);
  }

  Program graph_program = {0};
  SourceInput graph_input = {0};
  bool lowered = z_program_graph_lower_to_program_with_source(&graph, path, &graph_program, &graph_input, diag);
  if (lowered) {
    z_set_check_target(target);
    lowered = input->allow_missing_main ? z_check_program_library(&graph_program, diag) : z_check_program(&graph_program, diag);
  }
  if (!lowered) {
    if (graph_input.source_file) z_map_source_diag(&graph_input, diag);
    if (diag && !diag->path) diag->path = path;
    graph_compile_pin_diag_paths(diag);
    z_free_program(&graph_program); z_free_source(&graph_input); z_program_graph_free(&graph);
    return false;
  }

  IrProgram graph_ir = z_lower_program_graph_with_source(&graph, input, target);
  bool graph_mir_valid = graph_ir.mir_valid;
  if (graph_mir_valid) *ir = graph_ir;
  else { z_free_ir_program(&graph_ir); *ir = z_lower_program_with_source(&graph_program, input, target); }
  z_free_program(program);
  *program = graph_program;
  graph_program = (Program){0};
  graph_compile_replace_source_map(input, &graph_input);
  z_free_source(&graph_input);
  input->program_graph_hash = z_strdup(graph.graph_hash ? graph.graph_hash : "");
  input->program_graph_module_identity = z_strdup(graph.module_identity ? graph.module_identity : "");
  if (source) {
    source->artifact = path;
    source->graph_hash = input->program_graph_hash;
    source->module_identity = input->program_graph_module_identity;
    source->lowering = graph_mir_valid ? "typed-program-graph-mir" : "program-graph-ast-mir";
    source->canonical_source = graph.canonical_source;
  }
  z_program_graph_free(&graph);
  return true;
}
