
#ifndef WESTON_MODULE_XWAYLAND_H
#define WESTON_MODULE_XWAYLAND_H

#ifdef __cplusplus
extern "C" {
#endif

#include "compositor.h"

struct weston_xwayland_module_config {
	struct weston_module_config base;

	void *user_data;
	pid_t (*spawn_xserver)(void *user_data, int display, int abstract_fd,
	                       int unix_fd, int sv_fd, int wm_fd);

	/* out */
	void (*xserver_exited)(struct weston_module *module, int status);
};

#ifdef __cplusplus
}
#endif

#endif
