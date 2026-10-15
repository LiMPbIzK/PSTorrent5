/*
 * prospero-lld.c - Windows launcher for the PS5 (x86_64-sie-ps5) linker.
 *
 * Why this exists:
 *   LLVM's clang driver for the `x86_64-sie-ps5` target (LLVM 20 and newer)
 *   always looks for a linker program named "prospero-lld" and ignores
 *   --ld-path.  lld, on the other hand, selects its flavour from argv[0]:
 *   an executable called "prospero-lld" is rejected with
 *   "lld is a generic driver".
 *
 *   The SDK ships this launcher for Windows but clang cannot execute it, so
 *   this is a minimal, source-reproducible replacement: it execs the real
 *   lld from PATH with argv[0] set to "ld.lld.exe" so lld picks the GNU/ELF
 *   driver, forwarding every argument clang passed untouched.
 *
 * Build (host tool, not a PS5 payload):
 *   clang --target=x86_64-w64-windows-gnu -O2 -o prospero-lld.exe prospero-lld.c
 *
 * Override the real linker with the PROSPERO_LLD environment variable.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <process.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	const char *const fallback = "ld.lld.exe";
	const char *linker = getenv("PROSPERO_LLD");
	const char **args;
	int i;

	if (!linker || !linker[0])
		linker = fallback;

	/* argv[0] must be the lld-flavoured name, otherwise lld refuses to run. */
	args = (const char **)malloc(sizeof(char *) * (size_t)(argc + 1));
	if (!args) {
		fprintf(stderr, "prospero-lld: out of memory\n");
		return 1;
	}

	args[0] = fallback;
	for (i = 1; i < argc; i++)
		args[i] = argv[i];
	args[argc] = NULL;

	_execvp(linker, args);

	fprintf(stderr, "prospero-lld: cannot execute '%s'\n", linker);
	return 1;
}
