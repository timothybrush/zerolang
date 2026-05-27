#ifndef ZERO_C_PROGRAM_GRAPH_COMMAND_H
#define ZERO_C_PROGRAM_GRAPH_COMMAND_H

#include <stdbool.h>

typedef struct {
  bool supports_out;
  const char *message;
  const char *expected;
  const char *actual;
  const char *help;
} ZProgramGraphOutputContract;

bool z_program_graph_command_kind_is_known(const char *kind);
bool z_program_graph_command_kind_uses_artifact_input(const char *kind);
bool z_program_graph_command_kind_supports_out(const char *kind);
ZProgramGraphOutputContract z_program_graph_command_output_contract(const char *kind);
bool z_program_graph_direct_command_uses_manifest_input(const char *command);

#endif
