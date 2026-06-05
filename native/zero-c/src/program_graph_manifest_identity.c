#include "program_graph_manifest.h"

#include <stdlib.h>

bool z_program_graph_manifest_module_identity(const char *input_path, char **out, ZDiag *diag) {
  if (out) *out = NULL;
  char *manifest_path = z_manifest_path_for_input(input_path);
  if (!manifest_path) return true;

  char *manifest = z_read_file(manifest_path, diag);
  if (!manifest) {
    if (diag) diag->path = z_strdup(manifest_path);
    free(manifest_path);
    return false;
  }

  ZManifest parsed = {0};
  if (!z_parse_manifest_json(manifest, &parsed, diag)) {
    if (diag) diag->path = z_strdup(manifest_path);
    z_free_manifest(&parsed);
    free(manifest);
    free(manifest_path);
    return false;
  }

  if (out && parsed.package_name && parsed.package_name[0]) {
    ZBuf identity;
    zbuf_init(&identity);
    zbuf_append(&identity, "package:");
    zbuf_append(&identity, parsed.package_name);
    if (parsed.package_version && parsed.package_version[0]) {
      zbuf_append_char(&identity, '@');
      zbuf_append(&identity, parsed.package_version);
    }
    *out = identity.data ? identity.data : z_strdup("");
  }

  z_free_manifest(&parsed);
  free(manifest);
  free(manifest_path);
  return true;
}
