/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
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
#include "module-xwayland.h"
#include "shared/helpers.h"


static int
handle_sigusr1(int signal_number, void *data)
{
	struct weston_xserver *wxs = data;

	/* We'd be safer if we actually had the struct
	 * signalfd_siginfo from the signalfd data and could verify
	 * this came from Xwayland.*/
	wxs->wm = weston_wm_create(wxs, wxs->wm_fd);
	wl_event_source_remove(wxs->sigusr1_source);

	return 1;
}

static int
weston_xserver_handle_event(int listen_fd, uint32_t mask, void *data)
{
	struct weston_xserver *wxs = data;
	int sv[2], wm[2];

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		weston_log("wl connection socketpair failed\n");
		return 1;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) < 0) {
		weston_log("X wm connection socketpair failed\n");
		return 1;
	}

	wxs->pid = wxs->spawn_xserver(wxs->user_data, wxs->display,
				 wxs->abstract_fd, wxs->unix_fd, sv[1], wm[1]);

	if (wxs->pid == -1) {
		weston_log( "failed to fork\n");
		return -1;
	} else if (wxs->pid != 0) {
		close(sv[1]);
		wxs->client = wl_client_create(wxs->wl_display, sv[0]);

		close(wm[1]);
		wxs->wm_fd = wm[0];
	}
	weston_log("forked X server, pid %d\n", wxs->pid);
	wl_event_source_remove(wxs->abstract_source);
	wl_event_source_remove(wxs->unix_source);

	return 1;
}

static void
weston_xserver_shutdown(struct weston_xserver *wxs)
{
	char path[256];

	snprintf(path, sizeof path, "/tmp/.X%d-lock", wxs->display);
	unlink(path);
	snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", wxs->display);
	unlink(path);
	if (wxs->pid == 0) {
		wl_event_source_remove(wxs->abstract_source);
		wl_event_source_remove(wxs->unix_source);
	}
	close(wxs->abstract_fd);
	close(wxs->unix_fd);
	if (wxs->wm) {
		weston_wm_destroy(wxs->wm);
		wxs->wm = NULL;
	}
	wxs->loop = NULL;
}

static void
weston_xserver_exited(struct weston_xserver *wxs, int status)
{
	wxs->client = NULL;
	wxs->resource = NULL;

	wxs->abstract_source =
		wl_event_loop_add_fd(wxs->loop, wxs->abstract_fd,
				     WL_EVENT_READABLE,
				     weston_xserver_handle_event, wxs);

	wxs->unix_source =
		wl_event_loop_add_fd(wxs->loop, wxs->unix_fd,
				     WL_EVENT_READABLE,
				     weston_xserver_handle_event, wxs);

	if (wxs->wm) {
		weston_log("xserver exited, code %d\n", status);
		weston_wm_destroy(wxs->wm);
		wxs->wm = NULL;
	} else {
		/* If the X server crashes before it binds to the
		 * xserver interface, shut down and don't try
		 * again. */
		weston_log("xserver crashing too fast: %d\n", status);
		weston_xserver_shutdown(wxs);
	}
}

static int
bind_to_abstract_socket(int display)
{
	struct sockaddr_un addr;
	socklen_t size, name_size;
	int fd;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof addr.sun_path,
			     "%c/tmp/.X11-unix/X%d", 0, display);
	size = offsetof(struct sockaddr_un, sun_path) + name_size;
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		weston_log("failed to bind to @%s: %m\n", addr.sun_path + 1);
		close(fd);
		return -1;
	}

	if (listen(fd, 1) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int
bind_to_unix_socket(int display)
{
	struct sockaddr_un addr;
	socklen_t size, name_size;
	int fd;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof addr.sun_path,
			     "/tmp/.X11-unix/X%d", display) + 1;
	size = offsetof(struct sockaddr_un, sun_path) + name_size;
	unlink(addr.sun_path);
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		weston_log("failed to bind to %s: %m\n", addr.sun_path);
		close(fd);
		return -1;
	}

	if (listen(fd, 1) < 0) {
		unlink(addr.sun_path);
		close(fd);
		return -1;
	}

	return fd;
}

static int
create_lockfile(int display, char *lockfile, size_t lsize)
{
	char pid[16], *end;
	int fd, size;
	pid_t other;

	snprintf(lockfile, lsize, "/tmp/.X%d-lock", display);
	fd = open(lockfile, O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL, 0444);
	if (fd < 0 && errno == EEXIST) {
		fd = open(lockfile, O_CLOEXEC | O_RDONLY);
		if (fd < 0 || read(fd, pid, 11) != 11) {
			weston_log("can't read lock file %s: %s\n",
				lockfile, strerror(errno));
			if (fd >= 0)
				close (fd);

			errno = EEXIST;
			return -1;
		}

		other = strtol(pid, &end, 0);
		if (end != pid + 10) {
			weston_log("can't parse lock file %s\n",
				lockfile);
			close(fd);
			errno = EEXIST;
			return -1;
		}

		if (kill(other, 0) < 0 && errno == ESRCH) {
			/* stale lock file; unlink and try again */
			weston_log("unlinking stale lock file %s\n", lockfile);
			close(fd);
			if (unlink(lockfile))
				/* If we fail to unlink, return EEXIST
				   so we try the next display number.*/
				errno = EEXIST;
			else
				errno = EAGAIN;
			return -1;
		}

		close(fd);
		errno = EEXIST;
		return -1;
	} else if (fd < 0) {
		weston_log("failed to create lock file %s: %s\n",
			lockfile, strerror(errno));
		return -1;
	}

	/* Subtle detail: we use the pid of the wayland
	 * compositor, not the xserver in the lock file. */
	size = snprintf(pid, sizeof pid, "%10d\n", getpid());
	if (write(fd, pid, size) != size) {
		unlink(lockfile);
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static void
weston_xserver_destroy(struct weston_xserver *wxs)
{
	if (wxs->loop)
		weston_xserver_shutdown(wxs);

	free(wxs);
}

static void
xserver_destroy(struct wl_listener *l, void *data)
{
	struct weston_xserver *wxs =
		container_of(l, struct weston_xserver, destroy_listener);

	if (!wxs)
		return;

	weston_xserver_destroy(wxs);
}

static struct weston_xserver *
weston_xserver_create(struct weston_compositor *compositor)
{
	struct wl_display *display = compositor->wl_display;
	struct weston_xserver *wxs;
	char lockfile[256], display_name[8];

	wxs = zalloc(sizeof *wxs);
	if (wxs == NULL)
		return NULL;
	wxs->wl_display = display;
	wxs->compositor = compositor;

	wxs->display = 0;

retry:
	if (create_lockfile(wxs->display, lockfile, sizeof lockfile) < 0) {
		if (errno == EAGAIN) {
			goto retry;
		} else if (errno == EEXIST) {
			wxs->display++;
			goto retry;
		} else {
			free(wxs);
			return NULL;
		}
	}

	wxs->abstract_fd = bind_to_abstract_socket(wxs->display);
	if (wxs->abstract_fd < 0 && errno == EADDRINUSE) {
		wxs->display++;
		unlink(lockfile);
		goto retry;
	}

	wxs->unix_fd = bind_to_unix_socket(wxs->display);
	if (wxs->unix_fd < 0) {
		unlink(lockfile);
		close(wxs->abstract_fd);
		free(wxs);
		return NULL;
	}

	snprintf(display_name, sizeof display_name, ":%d", wxs->display);
	weston_log("xserver listening on display %s\n", display_name);
	setenv("DISPLAY", display_name, 1);

	wxs->loop = wl_display_get_event_loop(display);
	wxs->abstract_source =
		wl_event_loop_add_fd(wxs->loop, wxs->abstract_fd,
				     WL_EVENT_READABLE,
				     weston_xserver_handle_event, wxs);
	wxs->unix_source =
		wl_event_loop_add_fd(wxs->loop, wxs->unix_fd,
				     WL_EVENT_READABLE,
				     weston_xserver_handle_event, wxs);

	wxs->sigusr1_source = wl_event_loop_add_signal(wxs->loop, SIGUSR1,
						       handle_sigusr1, wxs);
	wxs->destroy_listener.notify = xserver_destroy;
	wl_signal_add(&compositor->destroy_signal, &wxs->destroy_listener);

	return wxs;
}

static void
exited(struct weston_module *module, int status)
{
	struct weston_xserver *wxs = (struct weston_xserver *)module;
	weston_xserver_exited(wxs, status);
}

struct weston_module *
module_init2(struct weston_compositor *compositor,
	     struct weston_module_config *base)
{
	struct weston_xwayland_module_config *config =
			(struct weston_xwayland_module_config *)base;
	struct weston_xserver *wxs;

	wxs = weston_xserver_create(compositor);
	if (!wxs)
		return NULL;

	wxs->user_data = config->user_data;
	wxs->spawn_xserver = config->spawn_xserver;
	config->xserver_exited = exited;
	return &wxs->base;
}

struct xserver_wrapper {
	struct weston_module *module;
	struct weston_process process;
	struct weston_config *config;
	void (*xserver_exited)(struct weston_module *module, int status);
};

static pid_t
wrapper_spawn_xserver(void *data, int dpy, int afd, int ufd, int svfd, int wmfd)
{
	struct xserver_wrapper *wrapper = data;
	char display[8], s[8], abstract_fd[8], unix_fd[8], wm_fd[8];
	int fd;
	char *xserver = NULL;
	struct weston_config_section *section;

	wrapper->process.pid = fork();
	switch (wrapper->process.pid) {
	case 0:
		/* SOCK_CLOEXEC closes both ends, so we need to unset
		 * the flag on the client fd. */
		fd = dup(svfd);
		if (fd < 0)
			goto fail;
		snprintf(s, sizeof s, "%d", fd);
		setenv("WAYLAND_SOCKET", s, 1);

		snprintf(display, sizeof display, ":%d", dpy);

		fd = dup(afd);
		if (fd < 0)
			goto fail;
		snprintf(abstract_fd, sizeof abstract_fd, "%d", fd);
		fd = dup(ufd);
		if (fd < 0)
			goto fail;
		snprintf(unix_fd, sizeof unix_fd, "%d", fd);
		fd = dup(wmfd);
		if (fd < 0)
			goto fail;
		snprintf(wm_fd, sizeof wm_fd, "%d", fd);

		section = weston_config_get_section(wrapper->config,
						    "xwayland", NULL, NULL);
		weston_config_section_get_string(section, "path",
						 &xserver, XSERVER_PATH);

		/* Ignore SIGUSR1 in the child, which will make the X
		 * server send SIGUSR1 to the parent (weston) when
		 * it's done with initialization.  During
		 * initialization the X server will round trip and
		 * block on the wayland compositor, so avoid making
		 * blocking requests (like xcb_connect_to_fd) until
		 * it's done with that. */
		signal(SIGUSR1, SIG_IGN);

		if (execl(xserver,
			  xserver,
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
                                   xserver, display,
                                   abstract_fd, unix_fd, wm_fd);
	fail:
		_exit(EXIT_FAILURE);

	default:
		weston_watch_process(&wrapper->process);
		break;

	case -1:
		break;
	}

	return 1;
}

static void
wrapper_cleanup(struct weston_process *process, int status)
{
	struct xserver_wrapper *wrapper =
		container_of(process, struct xserver_wrapper, process);

	wrapper->process.pid = 0;
	wrapper->xserver_exited(wrapper->module, status);
}

WL_EXPORT int
module_init(struct weston_compositor *compositor,
	    int *argc, char *argv[],
	    struct weston_config *config)

{
	struct xserver_wrapper *wrapper;
	struct weston_xwayland_module_config xwayland_config = {
		.spawn_xserver = wrapper_spawn_xserver,
	};

	wrapper = zalloc(sizeof *wrapper);
	if (!wrapper)
		return -1;

	xwayland_config.user_data = wrapper;

	wrapper->module = module_init2(compositor, &xwayland_config.base);
	if (!wrapper->module) {
		free(wrapper);
		return -1;
	}

	wrapper->xserver_exited = xwayland_config.xserver_exited;
	wrapper->config = config;
	wrapper->process.cleanup = wrapper_cleanup;

	return 0;
}
