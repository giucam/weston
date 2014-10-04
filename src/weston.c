/*
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2012-2014 Collabora, Ltd.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "config.h"

#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <linux/input.h>

#ifdef HAVE_LIBUNWIND
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#include "compositor.h"
#include "../shared/os-compatibility.h"
#include "git-version.h"
#include "version.h"
#include "screenshooter-server-protocol.h"

struct screenshooter {
	struct weston_compositor *ec;
	struct wl_global *global;
	struct wl_client *client;
	struct weston_process process;
	struct wl_listener destroy_listener;
};

static struct wl_list child_process_list;
static struct weston_compositor *segv_compositor;

static int
sigchld_handler(int signal_number, void *data)
{
	struct weston_process *p;
	int status;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		wl_list_for_each(p, &child_process_list, link) {
			if (p->pid == pid)
				break;
		}

		if (&p->link == &child_process_list) {
			weston_log("unknown child process exited\n");
			continue;
		}

		wl_list_remove(&p->link);
		p->cleanup(p, status);
	}

	if (pid < 0 && errno != ECHILD)
		weston_log("waitpid error %m\n");

	return 1;
}

static FILE *weston_logfile = NULL;

static int cached_tm_mday = -1;

static int weston_log_timestamp(void)
{
	struct timeval tv;
	struct tm *brokendown_time;
	char string[128];

	gettimeofday(&tv, NULL);

	brokendown_time = localtime(&tv.tv_sec);
	if (brokendown_time == NULL)
		return fprintf(weston_logfile, "[(NULL)localtime] ");

	if (brokendown_time->tm_mday != cached_tm_mday) {
		strftime(string, sizeof string, "%Y-%m-%d %Z", brokendown_time);
		fprintf(weston_logfile, "Date: %s\n", string);

		cached_tm_mday = brokendown_time->tm_mday;
	}

	strftime(string, sizeof string, "%H:%M:%S", brokendown_time);

	return fprintf(weston_logfile, "[%s.%03li] ", string, tv.tv_usec/1000);
}

static void
custom_handler(const char *fmt, va_list arg)
{
	weston_log_timestamp();
	fprintf(weston_logfile, "libwayland: ");
	vfprintf(weston_logfile, fmt, arg);
}

static void
weston_log_file_open(const char *filename)
{
	wl_log_set_handler_server(custom_handler);

	if (filename != NULL)
		weston_logfile = fopen(filename, "a");

	if (weston_logfile == NULL)
		weston_logfile = stderr;
	else
		setvbuf(weston_logfile, NULL, _IOLBF, 256);
}

static void
weston_log_file_close(void)
{
	if ((weston_logfile != stderr) && (weston_logfile != NULL))
		fclose(weston_logfile);
	weston_logfile = stderr;
}

static int
vlog(const char *fmt, va_list ap)
{
	int l;

	l = weston_log_timestamp();
	l += vfprintf(weston_logfile, fmt, ap);

	return l;
}

static int
vlog_continue(const char *fmt, va_list argp)
{
	return vfprintf(weston_logfile, fmt, argp);
}

static void
screenshooter_done(void *data, enum weston_screenshooter_outcome outcome)
{
	struct wl_resource *resource = data;

	switch (outcome) {
	case WESTON_SCREENSHOOTER_SUCCESS:
		screenshooter_send_done(resource);
		break;
	case WESTON_SCREENSHOOTER_NO_MEMORY:
		wl_resource_post_no_memory(resource);
		break;
	default:
		break;
	}
}

static void
screenshooter_shoot(struct wl_client *client,
		    struct wl_resource *resource,
		    struct wl_resource *output_resource,
		    struct wl_resource *buffer_resource)
{
	struct weston_output *output =
		wl_resource_get_user_data(output_resource);
	struct weston_buffer *buffer =
		weston_buffer_from_resource(buffer_resource);

	if (buffer == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	weston_screenshooter_shoot(output, buffer, screenshooter_done, resource);
}

struct screenshooter_interface screenshooter_implementation = {
	screenshooter_shoot
};

static void
bind_shooter(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	struct screenshooter *shooter = data;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &screenshooter_interface, 1, id);

	if (client != shooter->client) {
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "screenshooter failed: permission denied");
		return;
	}

	wl_resource_set_implementation(resource, &screenshooter_implementation,
				       data, NULL);
}

static void
screenshooter_sigchld(struct weston_process *process, int status)
{
	struct screenshooter *shooter =
		container_of(process, struct screenshooter, process);

	shooter->client = NULL;
}

static void
screenshooter_binding(struct weston_seat *seat, uint32_t time, uint32_t key,
		      void *data)
{
	struct screenshooter *shooter = data;
	char *screenshooter_exe;
	int ret;

	ret = asprintf(&screenshooter_exe, "%s/%s",
		       weston_config_get_libexec_dir(),
		       "weston-screenshooter");
	if (ret < 0) {
		weston_log("Could not construct screenshooter path.\n");
		return;
	}

	if (!shooter->client)
		shooter->client = weston_client_launch(shooter->ec,
					&shooter->process,
					screenshooter_exe, screenshooter_sigchld);
	free(screenshooter_exe);
}

static void
recorder_binding(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_output *output;
	int *running = data;

	if (*running == 1) {
		weston_recorder_stop(ec);
		*running = 0;
	} else {
		if (seat->keyboard && seat->keyboard->focus &&
		    seat->keyboard->focus->output)
			output = seat->keyboard->focus->output;
		else
			output = container_of(ec->output_list.next,
					      struct weston_output, link);

		weston_recorder_start(ec, output);
		*running = 1;
	}
}

static void
screenshooter_destroy(struct wl_listener *listener, void *data)
{
	struct screenshooter *shooter =
		container_of(listener, struct screenshooter, destroy_listener);

	wl_global_destroy(shooter->global);
	free(shooter);
}

static void
screenshooter_create(struct weston_compositor *ec)
{
	static int recorder_running = 0;
	struct screenshooter *shooter;

	shooter = malloc(sizeof *shooter);
	if (shooter == NULL)
		return;

	shooter->ec = ec;
	shooter->client = NULL;

	shooter->global = wl_global_create(ec->wl_display,
					   &screenshooter_interface, 1,
					   shooter, bind_shooter);
	weston_compositor_add_key_binding(ec, KEY_S, MODIFIER_SUPER,
					  screenshooter_binding, shooter);
	weston_compositor_add_key_binding(ec, KEY_R, MODIFIER_SUPER,
					  recorder_binding, &recorder_running);

	shooter->destroy_listener.notify = screenshooter_destroy;
	wl_signal_add(&ec->destroy_signal, &shooter->destroy_listener);
}

#ifdef HAVE_LIBUNWIND

static void
print_backtrace(void)
{
	unw_cursor_t cursor;
	unw_context_t context;
	unw_word_t off;
	unw_proc_info_t pip;
	int ret, i = 0;
	char procname[256];
	const char *filename;
	Dl_info dlinfo;

	pip.unwind_info = NULL;
	ret = unw_getcontext(&context);
	if (ret) {
		weston_log("unw_getcontext: %d\n", ret);
		return;
	}

	ret = unw_init_local(&cursor, &context);
	if (ret) {
		weston_log("unw_init_local: %d\n", ret);
		return;
	}

	ret = unw_step(&cursor);
	while (ret > 0) {
		ret = unw_get_proc_info(&cursor, &pip);
		if (ret) {
			weston_log("unw_get_proc_info: %d\n", ret);
			break;
		}

		ret = unw_get_proc_name(&cursor, procname, 256, &off);
		if (ret && ret != -UNW_ENOMEM) {
			if (ret != -UNW_EUNSPEC)
				weston_log("unw_get_proc_name: %d\n", ret);
			procname[0] = '?';
			procname[1] = 0;
		}

		if (dladdr((void *)(pip.start_ip + off), &dlinfo) && dlinfo.dli_fname &&
		    *dlinfo.dli_fname)
			filename = dlinfo.dli_fname;
		else
			filename = "?";

		weston_log("%u: %s (%s%s+0x%x) [%p]\n", i++, filename, procname,
			   ret == -UNW_ENOMEM ? "..." : "", (int)off, (void *)(pip.start_ip + off));

		ret = unw_step(&cursor);
		if (ret < 0)
			weston_log("unw_step: %d\n", ret);
	}
}

#else

static void
print_backtrace(void)
{
	void *buffer[32];
	int i, count;
	Dl_info info;

	count = backtrace(buffer, ARRAY_LENGTH(buffer));
	for (i = 0; i < count; i++) {
		dladdr(buffer[i], &info);
		weston_log("  [%016lx]  %s  (%s)\n",
			(long) buffer[i],
			info.dli_sname ? info.dli_sname : "--",
			info.dli_fname);
	}
}

#endif

WL_EXPORT void
weston_watch_process(struct weston_process *process)
{
	wl_list_insert(&child_process_list, &process->link);
}

static void
child_client_exec(int sockfd, const char *path)
{
	int clientfd;
	char s[32];
	sigset_t allsigs;

	/* do not give our signal mask to the new process */
	sigfillset(&allsigs);
	sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

	/* Launch clients as the user. Do not lauch clients with wrong euid.*/
	if (seteuid(getuid()) == -1) {
		weston_log("compositor: failed seteuid\n");
		return;
	}

	/* SOCK_CLOEXEC closes both ends, so we dup the fd to get a
	 * non-CLOEXEC fd to pass through exec. */
	clientfd = dup(sockfd);
	if (clientfd == -1) {
		weston_log("compositor: dup failed: %m\n");
		return;
	}

	snprintf(s, sizeof s, "%d", clientfd);
	setenv("WAYLAND_SOCKET", s, 1);

	if (execl(path, path, NULL) < 0)
		weston_log("compositor: executing '%s' failed: %m\n",
			path);
}

WL_EXPORT struct wl_client *
weston_client_launch(struct weston_compositor *compositor,
		     struct weston_process *proc,
		     const char *path,
		     weston_process_cleanup_func_t cleanup)
{
	int sv[2];
	pid_t pid;
	struct wl_client *client;

	weston_log("launching '%s'\n", path);

	if (os_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		weston_log("weston_client_launch: "
			"socketpair failed while launching '%s': %m\n",
			path);
		return NULL;
	}

	pid = fork();
	if (pid == -1) {
		close(sv[0]);
		close(sv[1]);
		weston_log("weston_client_launch: "
			"fork failed while launching '%s': %m\n", path);
		return NULL;
	}

	if (pid == 0) {
		child_client_exec(sv[1], path);
		_exit(-1);
	}

	close(sv[1]);

	client = wl_client_create(compositor->wl_display, sv[0]);
	if (!client) {
		close(sv[0]);
		weston_log("weston_client_launch: "
			"wl_client_create failed while launching '%s'.\n",
			path);
		return NULL;
	}

	proc->pid = pid;
	proc->cleanup = cleanup;
	weston_watch_process(proc);

	return client;
}

struct process_info {
	struct weston_process proc;
	char *path;
};

static void
process_handle_sigchld(struct weston_process *process, int status)
{
	struct process_info *pinfo =
		container_of(process, struct process_info, proc);

	/*
	 * There are no guarantees whether this runs before or after
	 * the wl_client destructor.
	 */

	if (WIFEXITED(status)) {
		weston_log("%s exited with status %d\n", pinfo->path,
			   WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		weston_log("%s died on signal %d\n", pinfo->path,
			   WTERMSIG(status));
	} else {
		weston_log("%s disappeared\n", pinfo->path);
	}

	free(pinfo->path);
	free(pinfo);
}

WL_EXPORT struct wl_client *
weston_client_start(struct weston_compositor *compositor, const char *path)
{
	struct process_info *pinfo;
	struct wl_client *client;

	pinfo = zalloc(sizeof *pinfo);
	if (!pinfo)
		return NULL;

	pinfo->path = strdup(path);
	if (!pinfo->path)
		goto out_free;

	client = weston_client_launch(compositor, &pinfo->proc, path,
				      process_handle_sigchld);
	if (!client)
		goto out_str;

	return client;

out_str:
	free(pinfo->path);

out_free:
	free(pinfo);

	return NULL;
}

static void
log_uname(void)
{
	struct utsname usys;

	uname(&usys);

	weston_log("OS: %s, %s, %s, %s\n", usys.sysname, usys.release,
						usys.version, usys.machine);
}

static const char xdg_error_message[] =
	"fatal: environment variable XDG_RUNTIME_DIR is not set.\n";

static const char xdg_wrong_message[] =
	"fatal: environment variable XDG_RUNTIME_DIR\n"
	"is set to \"%s\", which is not a directory.\n";

static const char xdg_wrong_mode_message[] =
	"warning: XDG_RUNTIME_DIR \"%s\" is not configured\n"
	"correctly.  Unix access mode must be 0700 (current mode is %o),\n"
	"and must be owned by the user (current owner is UID %d).\n";

static const char xdg_detail_message[] =
	"Refer to your distribution on how to get it, or\n"
	"http://www.freedesktop.org/wiki/Specifications/basedir-spec\n"
	"on how to implement it.\n";

static void
verify_xdg_runtime_dir(void)
{
	char *dir = getenv("XDG_RUNTIME_DIR");
	struct stat s;

	if (!dir) {
		weston_log(xdg_error_message);
		weston_log_continue(xdg_detail_message);
		exit(EXIT_FAILURE);
	}

	if (stat(dir, &s) || !S_ISDIR(s.st_mode)) {
		weston_log(xdg_wrong_message, dir);
		weston_log_continue(xdg_detail_message);
		exit(EXIT_FAILURE);
	}

	if ((s.st_mode & 0777) != 0700 || s.st_uid != getuid()) {
		weston_log(xdg_wrong_mode_message,
			   dir, s.st_mode & 0777, s.st_uid);
		weston_log_continue(xdg_detail_message);
	}
}

static int
usage(int error_code)
{
	fprintf(stderr,
		"Usage: weston [OPTIONS]\n\n"
		"This is weston version " VERSION ", the Wayland reference compositor.\n"
		"Weston supports multiple backends, and depending on which backend is in use\n"
		"different options will be accepted.\n\n"


		"Core options:\n\n"
		"  --version\t\tPrint weston version\n"
		"  -B, --backend=MODULE\tBackend module, one of\n"
#if defined(BUILD_DRM_COMPOSITOR)
			"\t\t\t\tdrm-backend.so\n"
#endif
#if defined(BUILD_FBDEV_COMPOSITOR)
			"\t\t\t\tfbdev-backend.so\n"
#endif
#if defined(BUILD_X11_COMPOSITOR)
			"\t\t\t\tx11-backend.so\n"
#endif
#if defined(BUILD_WAYLAND_COMPOSITOR)
			"\t\t\t\twayland-backend.so\n"
#endif
#if defined(BUILD_RDP_COMPOSITOR)
			"\t\t\t\trdp-backend.so\n"
#endif
#if defined(BUILD_RPI_COMPOSITOR) && defined(HAVE_BCM_HOST)
			"\t\t\t\trpi-backend.so\n"
#endif
		"  --shell=MODULE\tShell module, defaults to desktop-shell.so\n"
		"  -S, --socket=NAME\tName of socket to listen on\n"
		"  -i, --idle-time=SECS\tIdle time in seconds\n"
		"  --modules\t\tLoad the comma-separated list of modules\n"
		"  --log=FILE\t\tLog to the given file\n"
		"  --no-config\t\tDo not read weston.ini\n"
		"  -h, --help\t\tThis help message\n\n");

#if defined(BUILD_DRM_COMPOSITOR)
	fprintf(stderr,
		"Options for drm-backend.so:\n\n"
		"  --connector=ID\tBring up only this connector\n"
		"  --seat=SEAT\t\tThe seat that weston should run on\n"
		"  --tty=TTY\t\tThe tty to use\n"
		"  --use-pixman\t\tUse the pixman (CPU) renderer\n"
		"  --current-mode\tPrefer current KMS mode over EDID preferred mode\n\n");
#endif

#if defined(BUILD_FBDEV_COMPOSITOR)
	fprintf(stderr,
		"Options for fbdev-backend.so:\n\n"
		"  --tty=TTY\t\tThe tty to use\n"
		"  --device=DEVICE\tThe framebuffer device to use\n\n");
#endif

#if defined(BUILD_X11_COMPOSITOR)
	fprintf(stderr,
		"Options for x11-backend.so:\n\n"
		"  --width=WIDTH\t\tWidth of X window\n"
		"  --height=HEIGHT\tHeight of X window\n"
		"  --fullscreen\t\tRun in fullscreen mode\n"
		"  --use-pixman\t\tUse the pixman (CPU) renderer\n"
		"  --output-count=COUNT\tCreate multiple outputs\n"
		"  --no-input\t\tDont create input devices\n\n");
#endif

#if defined(BUILD_WAYLAND_COMPOSITOR)
	fprintf(stderr,
		"Options for wayland-backend.so:\n\n"
		"  --width=WIDTH\t\tWidth of Wayland surface\n"
		"  --height=HEIGHT\tHeight of Wayland surface\n"
		"  --scale=SCALE\t\tScale factor of output\n"
		"  --fullscreen\t\tRun in fullscreen mode\n"
		"  --use-pixman\t\tUse the pixman (CPU) renderer\n"
		"  --output-count=COUNT\tCreate multiple outputs\n"
		"  --sprawl\t\tCreate one fullscreen output for every parent output\n"
		"  --display=DISPLAY\tWayland display to connect to\n\n");
#endif

#if defined(BUILD_RPI_COMPOSITOR) && defined(HAVE_BCM_HOST)
	fprintf(stderr,
		"Options for rpi-backend.so:\n\n"
		"  --tty=TTY\t\tThe tty to use\n"
		"  --single-buffer\tUse single-buffered Dispmanx elements.\n"
		"  --transform=TR\tThe output transformation, TR is one of:\n"
		"\tnormal 90 180 270 flipped flipped-90 flipped-180 flipped-270\n"
		"  --opaque-regions\tEnable support for opaque regions, can be "
		"very slow without support in the GPU firmware.\n"
		"\n");
#endif

#if defined(BUILD_RDP_COMPOSITOR)
	fprintf(stderr,
		"Options for rdp-backend.so:\n\n"
		"  --width=WIDTH\t\tWidth of desktop\n"
		"  --height=HEIGHT\tHeight of desktop\n"
		"  --env-socket=SOCKET\tUse that socket as peer connection\n"
		"  --address=ADDR\tThe address to bind\n"
		"  --port=PORT\t\tThe port to listen on\n"
		"  --no-clients-resize\tThe RDP peers will be forced to the size of the desktop\n"
		"  --rdp4-key=FILE\tThe file containing the key for RDP4 encryption\n"
		"  --rdp-tls-cert=FILE\tThe file containing the certificate for TLS encryption\n"
		"  --rdp-tls-key=FILE\tThe file containing the private key for TLS encryption\n"
		"\n");
#endif

#if defined(BUILD_HEADLESS_COMPOSITOR)
	fprintf(stderr,
		"Options for headless-backend.so:\n\n"
		"  --width=WIDTH\t\tWidth of memory surface\n"
		"  --height=HEIGHT\tHeight of memory surface\n"
		"  --transform=TR\tThe output transformation, TR is one of:\n"
		"\tnormal 90 180 270 flipped flipped-90 flipped-180 flipped-270\n"
		"  --use-pixman\t\tUse the pixman (CPU) renderer (default: no rendering)\n\n");
#endif

	exit(error_code);
}

static int on_term_signal(int signal_number, void *data)
{
	struct wl_display *display = data;

	weston_log("caught signal %d\n", signal_number);
	wl_display_terminate(display);

	return 1;
}

static void
on_caught_signal(int s, siginfo_t *siginfo, void *context)
{
	/* This signal handler will do a best-effort backtrace, and
	 * then call the backend restore function, which will switch
	 * back to the vt we launched from or ungrab X etc and then
	 * raise SIGTRAP.  If we run weston under gdb from X or a
	 * different vt, and tell gdb "handle *s* nostop", this
	 * will allow weston to switch back to gdb on crash and then
	 * gdb will catch the crash with SIGTRAP.*/

	weston_log("caught signal: %d\n", s);

	print_backtrace();

	segv_compositor->backend->restore(segv_compositor);

	raise(SIGTRAP);
}

static void
catch_signals(void)
{
	struct sigaction action;

	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	action.sa_sigaction = on_caught_signal;
	sigemptyset(&action.sa_mask);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
}

static const char *
clock_name(clockid_t clk_id)
{
	static const char *names[] = {
		[CLOCK_REALTIME] =		"CLOCK_REALTIME",
		[CLOCK_MONOTONIC] =		"CLOCK_MONOTONIC",
		[CLOCK_MONOTONIC_RAW] =		"CLOCK_MONOTONIC_RAW",
		[CLOCK_REALTIME_COARSE] =	"CLOCK_REALTIME_COARSE",
		[CLOCK_MONOTONIC_COARSE] =	"CLOCK_MONOTONIC_COARSE",
		[CLOCK_BOOTTIME] =		"CLOCK_BOOTTIME",
	};

	if (clk_id < 0 || (unsigned)clk_id >= ARRAY_LENGTH(names))
		return "unknown";

	return names[clk_id];
}

static const struct {
	uint32_t bit; /* enum weston_capability */
	const char *desc;
} capability_strings[] = {
	{ WESTON_CAP_ROTATION_ANY, "arbitrary surface rotation:" },
	{ WESTON_CAP_CAPTURE_YFLIP, "screen capture uses y-flip:" },
};

static void
weston_compositor_log_capabilities(struct weston_compositor *compositor)
{
	unsigned i;
	int yes;

	weston_log("Compositor capabilities:\n");
	for (i = 0; i < ARRAY_LENGTH(capability_strings); i++) {
		yes = compositor->capabilities & capability_strings[i].bit;
		weston_log_continue(STAMP_SPACE "%s %s\n",
				    capability_strings[i].desc,
				    yes ? "yes" : "no");
	}

	weston_log_continue(STAMP_SPACE "presentation clock: %s, id %d\n",
			    clock_name(compositor->presentation_clock),
			    compositor->presentation_clock);
}

static void
handle_primary_client_destroyed(struct wl_listener *listener, void *data)
{
	struct wl_client *client = data;

	weston_log("Primary client died.  Closing...\n");

	wl_display_terminate(wl_client_get_display(client));
}

static int
weston_create_listening_socket(struct wl_display *display, const char *socket_name)
{
	if (socket_name) {
		if (wl_display_add_socket(display, socket_name)) {
			weston_log("fatal: failed to add socket: %m\n");
			return -1;
		}
	} else {
		socket_name = wl_display_add_socket_auto(display);
		if (!socket_name) {
			weston_log("fatal: failed to add socket: %m\n");
			return -1;
		}
	}

	setenv("WAYLAND_DISPLAY", socket_name, 1);

	return 0;
}

static int
load_modules(struct weston_compositor *ec, const char *modules,
	     int *argc, char *argv[], struct weston_config *config)
{
	const char *p, *end;
	char buffer[256];
	int (*module_init)(struct weston_compositor *ec,
			   int *argc, char *argv[],
			   struct weston_config *config);

	if (modules == NULL)
		return 0;

	p = modules;
	while (*p) {
		end = strchrnul(p, ',');
		snprintf(buffer, sizeof buffer, "%.*s", (int) (end - p), p);
		module_init = weston_load_module(buffer, "module_init");
		if (module_init)
			module_init(ec, argc, argv, config);
		p = end;
		while (*p == ',')
			p++;

	}

	return 0;
}

static int
weston_compositor_init_config(struct weston_compositor *ec,
			      struct weston_config *config)
{
	struct xkb_rule_names xkb_names;
	struct weston_config_section *s;

	s = weston_config_get_section(config, "keyboard", NULL, NULL);
	weston_config_section_get_string(s, "keymap_rules",
					 (char **) &xkb_names.rules, NULL);
	weston_config_section_get_string(s, "keymap_model",
					 (char **) &xkb_names.model, NULL);
	weston_config_section_get_string(s, "keymap_layout",
					 (char **) &xkb_names.layout, NULL);
	weston_config_section_get_string(s, "keymap_variant",
					 (char **) &xkb_names.variant, NULL);
	weston_config_section_get_string(s, "keymap_options",
					 (char **) &xkb_names.options, NULL);

	if (weston_compositor_xkb_init(ec, &xkb_names) < 0)
		return -1;

	weston_config_section_get_int(s, "repeat-rate",
				      &ec->kb_repeat_rate, 40);
	weston_config_section_get_int(s, "repeat-delay",
				      &ec->kb_repeat_delay, 400);

	text_backend_init(ec, config);

	return 0;
}

static char *
weston_choose_default_backend(void)
{
	char *backend = NULL;

	if (getenv("WAYLAND_DISPLAY") || getenv("WAYLAND_SOCKET"))
		backend = strdup("wayland-backend.so");
	else if (getenv("DISPLAY"))
		backend = strdup("x11-backend.so");
	else
		backend = strdup(WESTON_NATIVE_BACKEND);

	return backend;
}

static const struct { const char *name; uint32_t token; } transforms[] = {
	{ "normal",     WL_OUTPUT_TRANSFORM_NORMAL },
	{ "90",         WL_OUTPUT_TRANSFORM_90 },
	{ "180",        WL_OUTPUT_TRANSFORM_180 },
	{ "270",        WL_OUTPUT_TRANSFORM_270 },
	{ "flipped",    WL_OUTPUT_TRANSFORM_FLIPPED },
	{ "flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90 },
	{ "flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
	{ "flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
};

WL_EXPORT int
weston_parse_transform(const char *transform, uint32_t *out)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(transforms); i++)
		if (strcmp(transforms[i].name, transform) == 0) {
			*out = transforms[i].token;
			return 0;
		}

	*out = WL_OUTPUT_TRANSFORM_NORMAL;
	return -1;
}

WL_EXPORT const char *
weston_transform_to_string(uint32_t output_transform)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(transforms); i++)
		if (transforms[i].token == output_transform)
			return transforms[i].name;

	return "<illegal value>";
}

static void
handle_terminate(struct weston_compositor *c)
{
	wl_display_terminate(c->wl_display);
}

int main(int argc, char *argv[])
{
	int ret = EXIT_SUCCESS;
	struct wl_display *display;
	struct weston_compositor *ec;
	struct wl_event_source *signals[4];
	struct wl_event_loop *loop;
	int (*backend_init)(struct weston_compositor *c,
			    int *argc, char *argv[],
			    struct weston_config *config);
	int i, fd;
	char *backend = NULL;
	char *shell = NULL;
	char *modules = NULL;
	char *option_modules = NULL;
	char *log = NULL;
	char *server_socket = NULL, *end;
	int32_t idle_time = -1;
	int32_t help = 0;
	char *socket_name = NULL;
	int32_t version = 0;
	int32_t noconfig = 0;
	int32_t numlock_on;
	struct weston_config *config = NULL;
	struct weston_config_section *section;
	struct wl_client *primary_client;
	struct wl_listener primary_client_destroyed;
	struct weston_seat *seat;

	const struct weston_option core_options[] = {
		{ WESTON_OPTION_STRING, "backend", 'B', &backend },
		{ WESTON_OPTION_STRING, "shell", 0, &shell },
		{ WESTON_OPTION_STRING, "socket", 'S', &socket_name },
		{ WESTON_OPTION_INTEGER, "idle-time", 'i', &idle_time },
		{ WESTON_OPTION_STRING, "modules", 0, &option_modules },
		{ WESTON_OPTION_STRING, "log", 0, &log },
		{ WESTON_OPTION_BOOLEAN, "help", 'h', &help },
		{ WESTON_OPTION_BOOLEAN, "version", 0, &version },
		{ WESTON_OPTION_BOOLEAN, "no-config", 0, &noconfig },
	};

	parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);

	if (help)
		usage(EXIT_SUCCESS);

	if (version) {
		printf(PACKAGE_STRING "\n");
		return EXIT_SUCCESS;
	}

	weston_log_set_handler(vlog, vlog_continue);
	weston_log_file_open(log);

	weston_log("%s\n"
		   STAMP_SPACE "%s\n"
		   STAMP_SPACE "Bug reports to: %s\n"
		   STAMP_SPACE "Build: %s\n",
		   PACKAGE_STRING, PACKAGE_URL, PACKAGE_BUGREPORT,
		   BUILD_ID);
	log_uname();

	verify_xdg_runtime_dir();

	display = wl_display_create();

	loop = wl_display_get_event_loop(display);
	signals[0] = wl_event_loop_add_signal(loop, SIGTERM, on_term_signal,
					      display);
	signals[1] = wl_event_loop_add_signal(loop, SIGINT, on_term_signal,
					      display);
	signals[2] = wl_event_loop_add_signal(loop, SIGQUIT, on_term_signal,
					      display);

	wl_list_init(&child_process_list);
	signals[3] = wl_event_loop_add_signal(loop, SIGCHLD, sigchld_handler,
					      NULL);

	if (!signals[0] || !signals[1] || !signals[2] || !signals[3]) {
		ret = EXIT_FAILURE;
		goto out_signals;
	}

	if (noconfig == 0)
		config = weston_config_parse("weston.ini");
	if (config != NULL) {
		weston_log("Using config file '%s'\n",
			   weston_config_get_full_path(config));
	} else {
		weston_log("Starting with no config file.\n");
	}
	section = weston_config_get_section(config, "core", NULL, NULL);

	if (!backend) {
		weston_config_section_get_string(section, "backend", &backend,
						 NULL);
		if (!backend)
			backend = weston_choose_default_backend();
	}

	backend_init = weston_load_module(backend, "backend_init");
	if (!backend_init) {
		ret = EXIT_FAILURE;
		goto out_signals;
	}

	ec = zalloc(sizeof *ec);
	if (ec == NULL) {
		weston_log("fatal: failed to create compositor\n");
		ret = EXIT_FAILURE;
		goto out_signals;
	}

	ec->wl_display = display;
	if (weston_compositor_init(ec) < 0 ||
	    weston_compositor_init_config(ec, config) < 0) {
		ret = EXIT_FAILURE;
		goto out_signals;
	}

	if (backend_init(ec, &argc, argv, config) < 0) {
		weston_log("fatal: failed to create compositor backend\n");
		ret = EXIT_FAILURE;
		goto out_signals;
	}

	catch_signals();
	segv_compositor = ec;

	if (idle_time < 0)
		weston_config_section_get_int(section, "idle-time", &idle_time, -1);
	if (idle_time < 0)
		idle_time = 300; /* default idle timeout, in seconds */

	ec->idle_time = idle_time;
	ec->default_pointer_grab = NULL;
	ec->exit_code = EXIT_SUCCESS;
	ec->terminate = handle_terminate;

	for (i = 1; i < argc; i++)
		weston_log("fatal: unhandled option: %s\n", argv[i]);
	if (argc > 1) {
		ret = EXIT_FAILURE;
		goto out;
	}

	weston_compositor_log_capabilities(ec);

	server_socket = getenv("WAYLAND_SERVER_SOCKET");
	if (server_socket) {
		weston_log("Running with single client\n");
		fd = strtol(server_socket, &end, 0);
		if (*end != '\0')
			fd = -1;
	} else {
		fd = -1;
	}

	if (fd != -1) {
		primary_client = wl_client_create(display, fd);
		if (!primary_client) {
			weston_log("fatal: failed to add client: %m\n");
			ret = EXIT_FAILURE;
			goto out;
		}
		primary_client_destroyed.notify =
			handle_primary_client_destroyed;
		wl_client_add_destroy_listener(primary_client,
					       &primary_client_destroyed);
	} else if (weston_create_listening_socket(display, socket_name)) {
		ret = EXIT_FAILURE;
		goto out;
	}

	if (!shell)
		weston_config_section_get_string(section, "shell", &shell,
						 "desktop-shell.so");

	if (load_modules(ec, shell, &argc, argv, config) < 0)
		goto out;

	weston_config_section_get_string(section, "modules", &modules, "");
	if (load_modules(ec, modules, &argc, argv, config) < 0)
		goto out;

	if (load_modules(ec, option_modules, &argc, argv, config) < 0)
		goto out;

	section = weston_config_get_section(config, "keyboard", NULL, NULL);
	weston_config_section_get_bool(section, "numlock-on", &numlock_on, 0);
	if (numlock_on) {
		wl_list_for_each(seat, &ec->seat_list, link) {
			if (seat->keyboard)
				weston_keyboard_set_locks(seat->keyboard,
							  WESTON_NUM_LOCK,
							  WESTON_NUM_LOCK);
		}
	}

	screenshooter_create(ec);

	weston_compositor_wake(ec);

	wl_display_run(display);

	/* Allow for setting return exit code after
	* wl_display_run returns normally. This is
	* useful for devs/testers and automated tests
	* that want to indicate failure status to
	* testing infrastructure above
	*/
	ret = ec->exit_code;

out:
	weston_compositor_destroy(ec);

out_signals:
	for (i = ARRAY_LENGTH(signals) - 1; i >= 0; i--)
		if (signals[i])
			wl_event_source_remove(signals[i]);

	wl_display_destroy(display);

	weston_log_file_close();
	weston_config_destroy(config);

	free(backend);
	free(shell);
	free(socket_name);
	free(option_modules);
	free(log);
	free(modules);

	return ret;
}
