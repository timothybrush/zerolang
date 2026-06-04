#ifndef ZERO_C_PROGRAM_GRAPH_PROJECTION_H
#define ZERO_C_PROGRAM_GRAPH_PROJECTION_H

#include "program_graph_store.h"

typedef struct {
  char **changed_paths;
  size_t changed_len;
  size_t changed_cap;
  size_t source_count;
  size_t unchanged_count;
} ZProgramGraphProjection;

void z_program_graph_projection_init(ZProgramGraphProjection *projection);
void z_program_graph_projection_free(ZProgramGraphProjection *projection);
bool z_program_graph_projection_sources_match(const ZProgramGraphStore *store, bool *matches, ZDiag *diag);
bool z_program_graph_projection_write_sources(const ZProgramGraphStore *store, ZProgramGraphProjection *projection, ZDiag *diag);

#endif
