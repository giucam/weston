
#ifndef WESTON_COMPOSITOR_WAYLAND_H
#define WESTON_COMPOSITOR_WAYLAND_H

#ifdef  __cplusplus
extern "C" {
#endif

#include "compositor.h"

struct weston_compositor;
struct weston_seat;
struct wayland_backend;
struct wayland_output;

enum wayland_backend_fullscreen_method {
	WESTON_WAYLAND_BACKEND_FULLSCREEN_METHOD_DEFAULT = 0,
	WESTON_WAYLAND_BACKEND_FULLSCREEN_METHOD_SCALE = 1,
	WESTON_WAYLAND_BACKEND_FULLSCREEN_METHOD_DRIVER = 2,
	WESTON_WAYLAND_BACKEND_FULLSCREEN_METHOD_FILL = 3,
};

struct wayland_backend *
wayland_backend_create(struct weston_compositor *c, int use_pixman,
		       const char *display_name,
		       const char *cursor_theme, int cursor_size,
		       int sprawl);

struct wayland_output *
wayland_output_create(struct wayland_backend *b, int x, int y,
		      int width, int height, const char *name, int fullscreen,
		      uint32_t transform, int32_t scale);
int
wayland_output_is_fullscreen(struct wayland_output *output);
int
wayland_output_set_windowed(struct wayland_output *output);
void
wayland_output_set_fullscreen(struct wayland_output *output,
			      enum wayland_backend_fullscreen_method method,
			      uint32_t framerate, struct wl_output *target);
struct weston_output *
wayland_output_get_base(struct wayland_output *o);
struct wayland_output *
wayland_backend_find_output(struct wayland_backend *b,
			    struct weston_seat *seat);

#ifdef  __cplusplus
}
#endif

#endif
