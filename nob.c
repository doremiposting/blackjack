#include <stdio.h>
#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "nob.h"

#define CC "gcc"
#define forceinline inline __attribute__((__always_inline__))

Cmd cmd = {0};

static forceinline
int
boilerplate() {
	cmd_append(&cmd, CC, "-Wall", "-Wextra", "-Wformat", "-Wformat=2",
		"-Wconversion", "-Wsign-conversion", "-Werror=format-security",
		"-Wimplicit-fallthrough", "-Werror=implicit",
		"-Werror=incompatible-pointer-types", "-Werror=int-conversion",
		"-Wtrampolines", "-fzero-init-padding-bits=all", "-Wbidi-chars=any",
	);
}

static forceinline
int
compilefile(const char *fn, const char *out) {
  cmd_append(&cmd,
		"-std=c2y", "-g", "-c", "-o", out, fn
  );
	if (!cmd_run(&cmd)) { return 1; }
}

int
main(int argc, char *argv[]) {
	GO_REBUILD_URSELF(argc, argv); 

  if (!mkdir_if_not_exists("./build/")) { return 1; }

  boilerplate();
  cmd_append(&cmd, "-I/usr/include/freetype2");
  cmd_append(&cmd, "-lX11", "-lXft");
  cmd_append(&cmd, "-lm");
  cmd_append(&cmd, "-D_GNU_SOURCE");
  compilefile("src/main.c", "build/main.o");


	cmd_append(&cmd, CC, "-g", "-fPIE", "-pie", "-o", "bj",
    "-lX11", "-L/opt/X11/lib/", "-lXft", "-lm",
    "build/main.o");
	if (!cmd_run(&cmd)) { return 1; }
}
