#include "cli_help.h"

#include "program_graph_command.h"
#include "program_graph_patch.h"
#include "zero.h"

#include <stdio.h>
#include <string.h>

static bool cli_help_arg_is(const char *arg, const char *expected) {
  return strcmp(arg ? arg : "", expected) == 0;
}

static bool cli_help_is_program_graph_root_command(const char *command) {
  static const char *const commands[] = {"init", "dump", "import", "export", "query", "inspect", "validate", "view", "diff", "source-map", "reconcile", "status", "verify-projection", "merge", "roundtrip", "patch"};
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
    if (cli_help_arg_is(command, commands[i])) return true;
  }
  return false;
}

void z_cli_print_help(void) {
  printf("zero %s native bootstrap\n\n", ZERO_VERSION);
  fputs("Usage:\n  zero --version [--json]\n  zero skills [list|get] [--json]\n  zero new cli|lib|package <name>\n  zero check [--json] [--target <target>] [--emit exe|obj|llvm-ir] [graph-input]\n  zero patch [--json] [--check-only|--dry-run] [--format text|binary] [--out <program-graph-artifact>] [graph-input] (<patch-file>|--op <operation>)\n  zero test [graph-input]\n  zero fmt <file.0|project|zero.toml|zero.json>\n  zero build [--json] [--emit exe|obj|llvm-ir] [--backend direct|llvm|<direct-emitter>] [--target <target>] [--profile debug|dev|release-fast|release-small|tiny|audit] [--release <profile>] [--out <file>] [graph-input]\n  zero run [--backend direct|llvm|<direct-emitter>] [--target <target>] [--profile debug|dev|release-fast|release-small|tiny|audit] [--release <profile>] [--out <file>] [graph-input] [-- args...]\n  zero ship [--json] [--target <target>] [--profile release-small|tiny|audit] [--out <file>] [graph-input]\n  zero tokens --json <file.0|project|zero.toml|zero.json>\n  zero parse --json <file.0|project|zero.toml|zero.json>\n  zero init [--json] [--manifest toml|json] [--format text|binary] [project-path]\n  zero query [--json] [--fn <name>] [--find <text>] [--refs <name>] [--calls <name>] [--node <id>] [graph-input]\n  zero view [--json] [--out <file.0>] [graph-input]\n  zero diff [graph-input]\n  zero status|verify-projection [--json] [project|zero.toml|zero.json|file.0]\n  zero import [--json] [--format text|binary] [--out <program-graph-artifact>] [project|zero.toml|zero.json|file.0]\n  zero export [--json] [project|zero.toml|zero.json|file.0]\n  zero dump|validate|roundtrip [--json] [--format text|binary] [--out <program-graph-artifact>] [graph-input]\n  zero source-map [--json] [graph-input]\n  zero reconcile [--json] <base-graph-input> --source <edited-file.0|project|zero.toml|zero.json>\n  zero merge --base <base-zero.graph> --left <left-zero.graph> --right <right-zero.graph> [--json] [--format text|binary] <project|zero.toml|zero.json|file.0>\n  zero doc [--json] [graph-input]\n  zero size [--json] [--out <artifact>] [graph-input]\n  zero mem [--json] [--target <target>] [graph-input]\n  zero dev [--json] [--trace] [graph-input]\n  zero time --json [graph-input]\n  zero abi check|dump [--json] [--target <target>] [graph-input]\n  zero explain [--json] <code>\n  zero fix --plan --json [graph-input]\n  zero doctor [--json]\n  zero clean [--all]\n  zero targets\n\nExamples:\n  zero new cli hello\n  zero run examples/add.graph\n  zero build --emit exe examples/hello.graph --out .zero/out/hello\n  zero ship --target linux-musl-x64 examples/hello.graph --out .zero/ship/hello\n  zero check --json examples/hello.graph\n  zero build --target linux-musl-x64 examples/memory-package\n", stdout);
}

void z_cli_print_graph_patch_help_text(void) {
  printf("program graph patch operations\n");
  printf("accepted by zero patch --op, --patch-text, and zero-program-graph-patch v1 files\n");
  const char *const *ops = z_program_graph_patch_operation_examples();
  for (size_t i = 0; ops[i]; i++) printf("  %s\n", ops[i]);
  printf("\nFor larger graph edits, put these lines in a patch file under /tmp or pass --patch-text.\n");
}

void z_cli_print_command_help(const char *command) {
  if (cli_help_arg_is(command, "new")) {
    printf("Usage: zero new cli|lib|package <name>\n\n");
    printf("Create a small projection-oriented starter template. For graph-first agent-authored packages, prefer `zero init [path]`.\n\n");
    printf("Examples:\n");
    printf("  zero new cli hello\n");
    printf("  zero new lib math-kit\n");
    printf("  zero new package demo\n");
  } else if (cli_help_arg_is(command, "skills")) {
    printf("Usage: zero skills [list|get] [--json]\n\n");
    printf("List and retrieve version-matched skill content for agents.\n\n");
    printf("Subcommands:\n");
    printf("  list                 list available skills (default)\n");
    printf("  get <name> [--full]  print bundled skill content\n");
    printf("  get --all            print every visible skill\n");
  } else if (cli_help_arg_is(command, "doctor")) {
    printf("Usage: zero doctor [--json]\n\n");
    printf("Check host, compiler, target toolchain, and docs/example readiness.\n");
  } else if (cli_help_arg_is(command, "clean")) {
    printf("Usage: zero clean [--all]\n\n");
    printf("Remove generated output while preserving compiler caches by default.\n\n");
    printf("Flags:\n");
    printf("  --all    remove broader .zero generated state while preserving .zero/bin\n");
  } else if (cli_help_arg_is(command, "check")) {
    printf("Usage: zero check [--json] [--target <target>] [--emit exe|obj|llvm-ir] [--backend direct|llvm|<direct-emitter>] [graph-input]\n\n");
    printf("Validate and typecheck graph-backed Zero input without emitting artifacts.\n");
  } else if (cli_help_arg_is(command, "patch")) {
    printf("Usage: zero patch [--json] [--check-only|--dry-run] [--format text|binary] [--out <program-graph-artifact>] [graph-input] (<patch-file>|--op <operation>)\n\n");
    z_cli_print_graph_patch_help_text();
  } else if (cli_help_arg_is(command, "build")) {
    printf("Usage: zero build [--json] [--emit exe|obj|llvm-ir] [--backend direct|llvm|<direct-emitter>] [--target <target>] [--profile debug|dev|release-fast|release-small|tiny|audit] [--release <profile>] [--out <file>] [graph-input]\n\n");
    printf("Build direct native executable or object artifacts.\n\n");
    printf("Example: zero build --release tiny --emit exe examples/hello.graph --out .zero/out/hello\n");
  } else if (cli_help_arg_is(command, "run")) {
    printf("Usage: zero run [--backend direct|llvm|<direct-emitter>] [--target <target>] [--profile debug|dev|release-fast|release-small|tiny|audit] [--release <profile>] [--out <file>] [graph-input] [-- args...]\n\n");
    printf("Build a host executable with the selected backend and run it. Direct is the default; LLVM is explicit and requires clang. Program stdout and stderr are passed through unchanged.\n\n");
    printf("Example: zero run examples/add.graph\n");
  } else if (cli_help_arg_is(command, "ship")) {
    printf("Usage: zero ship [--json] [--target <target>] [--profile release-small|tiny|audit] [--out <file>] [graph-input]\n\n");
    printf("Produce a deterministic release preview with a direct binary, stripped binary copy, checksum, archive manifest, debug-symbol metadata, size report, and SBOM placeholder.\n\n");
    printf("Example: zero ship --target linux-musl-x64 examples/hello.graph --out .zero/ship/hello\n");
  } else if (cli_help_arg_is(command, "test")) {
    printf("Usage: zero test [--json] [--filter <name>] [--target <target>] [--cc <path>] [--out <file>] [graph-input]\n\n");
    printf("Build and run inline `test` blocks.\n");
  } else if (cli_help_arg_is(command, "fmt")) {
    printf("Usage: zero fmt [--check] <file.0|project|zero.toml|zero.json>\n\n");
    printf("Print deterministic bootstrap formatting for Zero source.\n");
  } else if (cli_help_arg_is(command, "targets")) {
    printf("Usage: zero targets\n\n");
    printf("Print supported target facts as JSON.\n");
  } else if (cli_help_arg_is(command, "tokens")) {
    printf("Usage: zero tokens --json <file.0|project|zero.toml|zero.json>\n\n");
    printf("Emit source token JSON for oracle comparisons.\n");
  } else if (cli_help_arg_is(command, "parse")) {
    printf("Usage: zero parse --json <file.0|project|zero.toml|zero.json>\n\n");
    printf("Emit normalized source parse JSON for oracle comparisons.\n");
  } else if (cli_help_arg_is(command, "abi")) {
    printf("Usage: zero abi check|dump [--json] [--target <target>] [graph-input]\n\n");
    printf("Check ABI-safe declarations or dump target-aware graph layout facts.\n");
  } else if (cli_help_arg_is(command, "graph") || cli_help_is_program_graph_root_command(command)) {
    z_program_graph_print_command_help();
  } else if (cli_help_arg_is(command, "doc")) {
    printf("Usage: zero doc [--json] [--target <target>] [graph-input]\n\n");
    printf("Emit package API documentation facts without emitting artifacts.\n");
  } else if (cli_help_arg_is(command, "size")) {
    printf("Usage: zero size [--json] [--target <target>] [--profile debug|dev|release-fast|release-small|tiny|audit] [--release <profile>] [--out <artifact>] [graph-input]\n\n");
    printf("Report direct IR size, optional artifact bytes, capabilities, and stdlib helper metadata.\n");
  } else if (cli_help_arg_is(command, "mem")) {
    printf("Usage: zero mem [--json] [--target <target>] [graph-input]\n\n");
    printf("Report direct stack, static, heap, buffer, and runtime memory facts.\n");
  } else if (cli_help_arg_is(command, "dev")) {
    printf("Usage: zero dev [--json] [--trace] [--target <target>] [graph-input]\n\n");
    printf("Emit a direct incremental watch plan, interface fingerprints, and affected-test summary.\n");
  } else if (cli_help_arg_is(command, "time")) {
    printf("Usage: zero time --json [--target <target>] [graph-input]\n\n");
    printf("Emit compiler phase, cache, and invalidation timing facts.\n");
  } else if (cli_help_arg_is(command, "explain")) {
    printf("Usage: zero explain [--json] <diagnostic-code>\n\n");
    printf("Explain a diagnostic and its repair metadata.\n");
  } else if (cli_help_arg_is(command, "fix")) {
    printf("Usage: zero fix (--plan|--patch|--apply) --json [--target <target>] [graph-input]\n\n");
    printf("Print repair plans, reviewable patches, or apply behavior-preserving edits for graph-backed inputs.\n");
  } else if (cli_help_arg_is(command, "version") || cli_help_arg_is(command, "--version")) {
    printf("Usage: zero --version [--json]\n\n");
    printf("Print version, commit, host target, compiler backend, and target toolchain availability.\n");
  } else {
    z_cli_print_help();
  }
}
