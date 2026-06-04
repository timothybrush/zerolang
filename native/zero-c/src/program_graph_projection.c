#include "program_graph_projection.h"

#include "zero.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool projection_text_eq(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}

static char *projection_join_path(const char *left, const char *right) {
  ZBuf buf;
  zbuf_init(&buf);
  zbuf_append(&buf, left && left[0] ? left : ".");
  if (buf.len > 0 && buf.data[buf.len - 1] != '/') zbuf_append_char(&buf, '/');
  zbuf_append(&buf, right && right[0] ? right : ".");
  return buf.data;
}

static bool projection_add_changed_path(ZProgramGraphProjection *projection, const char *path) {
  if (!projection || !path || !path[0]) return true;
  if (projection->changed_len == projection->changed_cap) {
    size_t next = z_grow_capacity(projection->changed_cap, projection->changed_len + 1, 4);
    projection->changed_paths = z_checked_reallocarray(projection->changed_paths, next, sizeof(char *));
    projection->changed_cap = next;
  }
  projection->changed_paths[projection->changed_len++] = z_strdup(path);
  return true;
}

static bool projection_source_text(const ZProgramGraphStore *store, const char *source_path, const char **out, ZDiag *diag) {
  if (!z_program_graph_store_source_path_is_local(source_path)) {
    if (out) *out = NULL;
    if (diag) {
      diag->code = 1002;
      diag->path = store && store->path ? store->path : "zero.graph";
      diag->line = 1;
      diag->column = 1;
      diag->length = 1;
      snprintf(diag->message, sizeof(diag->message), "repository graph source projection path is not local");
      snprintf(diag->expected, sizeof(diag->expected), "relative source projection path inside the repository graph root");
      snprintf(diag->actual, sizeof(diag->actual), "%s", source_path && source_path[0] ? source_path : "missing source path");
    }
    return false;
  }
  const char *text = z_program_graph_store_projection_text(store, source_path);
  if (out) *out = text;
  if (!text) {
    if (diag) {
      diag->code = 1002;
      diag->path = store && store->path ? store->path : "zero.graph";
      diag->line = 1;
      diag->column = 1;
      diag->length = 1;
      snprintf(diag->message, sizeof(diag->message), "repository graph source projection is missing source text");
      snprintf(diag->expected, sizeof(diag->expected), "source projection text in zero.graph");
      snprintf(diag->actual, sizeof(diag->actual), "%s", source_path && source_path[0] ? source_path : "missing source path");
    }
    return false;
  }
  return true;
}

static bool projection_source_text_matches(const ZProgramGraphStore *store, const char *source_path, bool *matches, ZDiag *diag) {
  *matches = false;
  const char *projection = NULL;
  if (!projection_source_text(store, source_path, &projection, diag)) return false;
  char *path = projection_join_path(store->root, source_path);
  ZDiag read_diag = {0};
  char *current = z_read_file(path, &read_diag);
  if (current) *matches = projection_text_eq(current, projection);
  free(current);
  free(path);
  return true;
}

void z_program_graph_projection_init(ZProgramGraphProjection *projection) {
  if (projection) *projection = (ZProgramGraphProjection){0};
}

void z_program_graph_projection_free(ZProgramGraphProjection *projection) {
  if (!projection) return;
  for (size_t i = 0; i < projection->changed_len; i++) free(projection->changed_paths[i]);
  free(projection->changed_paths);
  *projection = (ZProgramGraphProjection){0};
}

bool z_program_graph_projection_sources_match(const ZProgramGraphStore *store, bool *matches, ZDiag *diag) {
  if (matches) *matches = false;
  if (!store || store->projection_len == 0) {
    if (diag) {
      diag->code = 1002;
      diag->path = store && store->path ? store->path : "zero.graph";
      diag->line = 1;
      diag->column = 1;
      diag->length = 1;
      snprintf(diag->message, sizeof(diag->message), "repository graph store has no source projections");
      snprintf(diag->expected, sizeof(diag->expected), "one or more source projection rows");
      snprintf(diag->actual, sizeof(diag->actual), "empty projection table");
    }
    return false;
  }
  bool all_match = true;
  for (size_t i = 0; i < store->projection_len; i++) {
    bool file_matches = false;
    if (!projection_source_text_matches(store, store->projection_paths[i], &file_matches, diag)) return false;
    if (!file_matches) all_match = false;
  }
  if (matches) *matches = all_match;
  return true;
}

bool z_program_graph_projection_write_sources(const ZProgramGraphStore *store, ZProgramGraphProjection *projection, ZDiag *diag) {
  if (!store || store->projection_len == 0) return z_program_graph_projection_sources_match(store, NULL, diag);
  if (projection) z_program_graph_projection_init(projection);
  for (size_t i = 0; i < store->projection_len; i++) {
    const char *source_path = store->projection_paths[i];
    const char *source_text = NULL;
    if (!projection_source_text(store, source_path, &source_text, diag)) return false;
    char *path = projection_join_path(store->root, source_path);
    ZDiag read_diag = {0};
    char *current = z_read_file(path, &read_diag);
    bool changed = !current || !projection_text_eq(current, source_text);
    free(current);
    if (projection) projection->source_count++;
    if (changed) {
      if (!z_write_file(path, source_text, diag)) {
        free(path);
        return false;
      }
      if (projection) projection_add_changed_path(projection, path);
    } else if (projection) {
      projection->unchanged_count++;
    }
    free(path);
  }
  return true;
}
