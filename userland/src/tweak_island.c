/*
 * RedwoodIsland: the island of this port, as a tweak of the jailbreak prefix.
 *
 * The port draws its own island -- a black pill over the status bar in SpringBoard, with the
 * MobileGestalt answers that say "this device has one" -- and it lives in
 * /redwood/lib/libredwood-sb.dylib, which init has the kernel insert into SpringBoard. This tweak
 * exists so that the same thing is a package: listed in Sileo, removable there, and loaded through
 * ElleKit's TweakLoader the way every other tweak is (the port's init inserts
 * /redwood/lib/libredwood-tweaks.dylib into SpringBoard, and that opens
 * /var/jb/usr/lib/TweakLoader.dylib, which reads the .plist filters beside the dylibs here).
 *
 * It does one thing: take the library that draws it. If the kernel already inserted it into this
 * process, dlopen() hands back the handle of the copy that is running and its constructor does not
 * run twice -- so this is safe whether or not the port injected it.
 *
 * Settled by boot argument, in the library itself: island=0 in the kernel's command line leaves the
 * island out and no_gestalt_hook=1 leaves MobileGestalt alone. Those are the port's switches, not
 * this package's; a tweak cannot see them until the library does.
 */
#include <dlfcn.h>
#include <stdio.h>

#define REDWOOD_SPRINGBOARD_LIBRARY "/redwood/lib/libredwood-sb.dylib"

__attribute__((constructor))
static void
redwood_island_load(void)
{
	void *handle;

	if (dlopen(REDWOOD_SPRINGBOARD_LIBRARY, RTLD_NOW | RTLD_GLOBAL) == NULL) {
		fprintf(stderr, "RedwoodIsland: %s: %s\n", REDWOOD_SPRINGBOARD_LIBRARY, dlerror());
		return;
	}
	handle = NULL;
	(void)handle;
}
