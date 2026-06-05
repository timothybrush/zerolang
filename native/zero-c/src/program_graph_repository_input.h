#ifndef ZERO_C_PROGRAM_GRAPH_REPOSITORY_INPUT_H
#define ZERO_C_PROGRAM_GRAPH_REPOSITORY_INPUT_H

#include "program_graph.h"

#include <stdbool.h>

int z_repository_graph_verify_compiler_input(const char *input, const ZTargetInfo *target, bool json, const ZProgramGraph *source_graph, const ZDiag *source_graph_diag, char **out_store_path);

#endif
