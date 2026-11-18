/*
 * RedwoodExc: say what every app's exception is, in the port's own log.
 *
 * Why it exists. Eleven processes of the 21:03 run (MobileMail, MailWidgetExtension x3, AppleTV,
 * Podcasts, Preferences, Music x3, PassbookWidgetsExtension) died about a second after launch, and
 * every one of their stacks, once the frames were looked up in the dyld shared cache, read the same:
 *
 *     abort            libsystem_c
 *     std::terminate   libc++abi
 *     objc_exception_throw           libobjc
 *     _dispatch_client_callout       libdispatch
 *     MediaPlayer +0x18804, +0x18888 MediaPlayer.framework   <- in all eleven
 *
 * So an Objective-C exception is thrown inside a block MediaPlayer hands to libdispatch, nobody
 * catches it, and the process aborts. The kernel's stack walk shows the frames but not the
 * exception -- its name and reason live in the object -- and an app's stderr does not reach the
 * kernel log. This tweak fills that gap: it interposes objc_exception_throw, writes the name, the
 * reason, the argument and the frames that threw it to /dev/console (which the kernel log does
 * carry), to stderr, and to a few files, so the next run says exactly which exception it is.
 *
 * Loaded through ElleKit like the island tweak; its .plist has an empty filter, i.e. every process.
 *
 * The recording is deliberately paranoid: it does nothing but read the exception's own strings and
 * write bytes. If any of that fails the process still dies of the exception it would have died of.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct objc_object *id;
typedef struct objc_selector *SEL;

extern void objc_exception_throw(id exception);

/* ---- where the words go ------------------------------------------------------------------- */

/* /dev/console first: on this port that write ends up in the kernel's message ring, which the
 * loader hands out with "oem log" -- i.e. in the log we already know how to fetch. The files are
 * for the case where somebody gets a shell in the guest; stderr for the case where launchd keeps it. */
static const char *const sinks[] = {
	"/dev/console",
	"/private/var/tmp/redwood-exc.log",
	"/var/tmp/redwood-exc.log",
	"/tmp/redwood-exc.log",
	"/redwood/redwood-exc.log",
	NULL
};

static int sink_fd[6];
static bool sink_open;

static void
open_sinks(void)
{
	if (sink_open) {
		return;
	}
	sink_open = true;
	for (int i = 0; sinks[i] != NULL; i++) {
		int flags = O_WRONLY | O_APPEND;
		if (i != 0) {
			flags |= O_CREAT;               /* /dev/console exists; the files may not */
		}
		sink_fd[i] = open(sinks[i], flags, 0644);
	}
}

/* one write, one line: a partially written line would be useless in a ring buffer */
static void
emit(const char *text)
{
	size_t length = strlen(text);

	open_sinks();
	(void)write(STDERR_FILENO, text, length);
	for (int i = 0; sinks[i] != NULL; i++) {
		if (sink_fd[i] > 0) {
			(void)write(sink_fd[i], text, length);
		}
	}
}

/* ---- reading the exception ---------------------------------------------------------------- */

static id (*msg_send_ptr)(id, SEL, ...);
static SEL (*sel_ptr)(const char *);

static const char *
text_of(id object, char *buffer, size_t size)
{
	SEL selector;
	id string;

	if (object == NULL || msg_send_ptr == NULL || sel_ptr == NULL) {
		return "(nil)";
	}
	selector = sel_ptr("description");
	if (selector == NULL) {
		return "(no description)";
	}
	string = ((id (*)(id, SEL))msg_send_ptr)(object, selector);
	if (string == NULL) {
		return "(nil description)";
	}
	selector = sel_ptr("UTF8String");
	if (selector == NULL) {
		return "(no UTF8String)";
	}
	const char *utf8 = ((const char *(*)(id, SEL))msg_send_ptr)(string, selector);
	if (utf8 == NULL) {
		return "(nil UTF8String)";
	}
	snprintf(buffer, size, "%s", utf8);
	return buffer;
}

static const char *
class_name_of(id object)
{
	id klass;

	if (object == NULL || msg_send_ptr == NULL || sel_ptr == NULL) {
		return "(nil)";
	}
	/* -class on an instance; -description of that class is its name */
	klass = ((id (*)(id, SEL))msg_send_ptr)(object, sel_ptr("class"));
	return text_of(klass, (char [160]){0}, 160);
}

/* The frames, from here up: enough to say "thrown by MediaPlayer, not by the app". Frame pointers
 * are kept by the tweaks' own build (-fno-omit-frame-pointer), and every caller's too on arm64. */
static void
append_frames(char *buffer, size_t size, size_t used)
{
	void *frame = __builtin_frame_address(0);
	int shown = 0;

	for (int i = 0; i < 14 && frame != NULL && used + 20 < size; i++) {
		void **words = (void **)frame;
		void *link = words[1];                  /* the return address */
		void *next = words[0];

		if (link != NULL) {
			used += (size_t)snprintf(buffer + used, size - used, " %p", link);
		}
		if (next == NULL || next <= frame) {
			break;
		}
		frame = next;
		shown++;
	}
	(void)shown;
}

/* ---- the interposed call ------------------------------------------------------------------ */

static volatile int inside;

extern void
redwood_exc_log_throw(id exception)
{
	char line[1024];
	char reason[512];
	char name[256];
	size_t used = 0;

	if (__sync_lock_test_and_set(&inside, 1) != 0) {
		return;                                 /* already saying something; do not recurse */
	}

	name[0] = '\0';
	reason[0] = '\0';
	if (exception != NULL && msg_send_ptr != NULL && sel_ptr != NULL) {
		SEL selector = sel_ptr("name");
		if (selector != NULL) {
			id string = ((id (*)(id, SEL))msg_send_ptr)(exception, selector);
			const char *utf8 = text_of(string, (char [256]){0}, 256);
			snprintf(name, sizeof(name), "%s", utf8);
		}
		selector = sel_ptr("reason");
		if (selector != NULL) {
			id string = ((id (*)(id, SEL))msg_send_ptr)(exception, selector);
			const char *utf8 = text_of(string, (char [512]){0}, 512);
			snprintf(reason, sizeof(reason), "%s", utf8);
		}
	}

	used = (size_t)snprintf(line, sizeof(line), "redwood.exc: %d threw <%s>: <%s> [%s]",
	    (int)getpid(), name, reason,
	    exception == NULL ? "nil" : class_name_of(exception));
	if (used >= sizeof(line) - 2) {
		used = sizeof(line) - 2;                /* snprintf truncated it; the newline still fits */
	}
	append_frames(line, sizeof(line) - 2, used);
	{
		size_t end = strlen(line);

		if (end > sizeof(line) - 2) {
			end = sizeof(line) - 2;
		}
		line[end] = '\n';
		line[end + 1] = '\0';
	}
	emit(line);

	__sync_lock_release(&inside);
}

/* dyld applies __DATA,__interpose to every image as it is loaded, which is how objc_exception_throw
 * gets here while libobjc itself keeps working for everybody else. */
struct interpose_pair {
	const void *replacement;
	const void *replacee;
};

__attribute__((used, section("__DATA,__interpose")))
static const struct interpose_pair redwood_exc_interposed[] = {
	{ (const void *)redwood_exc_log_throw, (const void *)objc_exception_throw },
};

/* ---- and the one that is never caught ------------------------------------------------------ */

static void
uncaught(id exception)
{
	char line[1024];
	char reason[512];

	snprintf(reason, sizeof(reason), "%s", text_of(exception, (char [512]){0}, 512));
	snprintf(line, sizeof(line), "redwood.exc: %d UNCAUGHT <%s>: <%s> [%s]\n",
	    (int)getpid(), class_name_of(exception), reason,
	    text_of(exception, (char [512]){0}, 512));
	emit(line);
}

__attribute__((constructor))
static void
redwood_exc_load(void)
{
	char line[192];

	msg_send_ptr = (id (*)(id, SEL, ...))dlsym(RTLD_DEFAULT, "objc_msgSend");
	sel_ptr = (SEL (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");

	/* the handler is worth having even if msgSend did not resolve: a name and a reason is what
	 * we want, and libobjc calls it for the exceptions it can still see */
	{
		void (*set_handler)(void (*)(id)) =
		    (void (*)(void (*)(id)))dlsym(RTLD_DEFAULT, "NSSetUncaughtExceptionHandler");
		if (set_handler != NULL) {
			set_handler(uncaught);
		}
	}

	snprintf(line, sizeof(line), "redwood.exc: %d loaded (msgSend %s, sel %s)\n",
	    (int)getpid(), msg_send_ptr != NULL ? "yes" : "NO", sel_ptr != NULL ? "yes" : "NO");
	emit(line);
}
