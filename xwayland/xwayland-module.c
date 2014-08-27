/*
 * Copyright © 2011 Intel Corporation
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include "xwayland.h"
#include "compositor.h"

struct xserver {
	struct wl_listener destroy_listener;
	struct weston_xserver *weston_xserver;
	struct weston_process process;
	char *xserver_path;
};

static void
xserver_destroy(struct wl_listener *l, void *data)
{
	struct xserver *xs =
		container_of(l, struct xserver, destroy_listener);

	if (!xs || !xs->weston_xserver)
		return;

	weston_xserver_destroy(xs->weston_xserver);
	free(xs->xserver_path);
	free(xs);
}

static pid_t
xserver_spawn(struct weston_xserver *wxs)
{
	struct wl_listener *l = wl_signal_get(&wxs->compositor->destroy_signal,
					      xserver_destroy);
	struct xserver *xs = container_of(l, struct xserver, destroy_listener);

	pid_t pid;
	char display[8], s[8], abstract_fd[8], unix_fd[8], wm_fd[8];
	int sv[2], wm[2], fd;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		weston_log("wl connection socketpair failed\n");
		return 1;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) < 0) {
		weston_log("X wm connection socketpair failed\n");
		return 1;
	}

	pid = fork();
	switch (pid) {
	case 0:
		/* SOCK_CLOEXEC closes both ends, so we need to unset
		 * the flag on the client fd. */
		fd = dup(sv[1]);
		if (fd < 0)
			goto fail;
		snprintf(s, sizeof s, "%d", fd);
		setenv("WAYLAND_SOCKET", s, 1);

		snprintf(display, sizeof display, ":%d", wxs->display);

		fd = dup(wxs->abstract_fd);
		if (fd < 0)
			goto fail;
		snprintf(abstract_fd, sizeof abstract_fd, "%d", fd);
		fd = dup(wxs->unix_fd);
		if (fd < 0)
			goto fail;
		snprintf(unix_fd, sizeof unix_fd, "%d", fd);
		fd = dup(wm[1]);
		if (fd < 0)
			goto fail;
		snprintf(wm_fd, sizeof wm_fd, "%d", fd);

		/* Ignore SIGUSR1 in the child, which will make the X
		 * server send SIGUSR1 to the parent (weston) when
		 * it's done with initialization.  During
		 * initialization the X server will round trip and
		 * block on the wayland compositor, so avoid making
		 * blocking requests (like xcb_connect_to_fd) until
		 * it's done with that. */
		signal(SIGUSR1, SIG_IGN);

		if (execl(xs->xserver_path,
			  xs->xserver_path,
			  display,
			  "-rootless",
			  "-listen", abstract_fd,
			  "-listen", unix_fd,
			  "-wm", wm_fd,
			  "-terminate",
			  NULL) < 0)
			weston_log("exec of '%s %s -rootless "
                                   "-listen %s -listen %s -wm %s "
                                   "-terminate' failed: %m\n",
                                   xs->xserver_path, display,
                                   abstract_fd, unix_fd, wm_fd);
	fail:
		_exit(EXIT_FAILURE);

	default:
		close(sv[1]);
		wxs->client = wl_client_create(wxs->wl_display, sv[0]);

		close(wm[1]);
		wxs->wm_fd = wm[0];

		xs->process.pid = pid;
		weston_watch_process(&xs->process);
		break;

	case -1:
		weston_log( "failed to fork\n");
		break;
	}

	return pid;
}

static void
xserver_cleanup(struct weston_process *process, int status)
{
	struct xserver *xs =
		container_of(process, struct xserver, process);
	weston_xserver_exited(xs->weston_xserver, status);
}

WL_EXPORT int
module_init(struct weston_compositor *compositor,
            int *argc, char *argv[], struct weston_config *config)

{
	struct xserver *xs;
	struct weston_config_section *section;

	xs = zalloc(sizeof *xs);
	if (!xs)
		return -1;

	xs->weston_xserver = weston_xserver_create(compositor);
	if (!xs->weston_xserver) {
		free(xs);
		return -1;
	}

	section = weston_config_get_section(config,
	                                    "xwayland", NULL, NULL);
	weston_config_section_get_string(section, "path",
	                                 &xs->xserver_path, XSERVER_PATH);

	xs->process.cleanup = xserver_cleanup;
	xs->weston_xserver->spawn_xserver = xserver_spawn;
	xs->destroy_listener.notify = xserver_destroy;
	wl_signal_add(&compositor->destroy_signal, &xs->destroy_listener);

	return 0;
}