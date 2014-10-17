
#ifndef WESTON_COMPOSITOR_X11_H
#define WESTON_COMPOSITOR_X11_H

#ifdef  __cplusplus
extern "C" {
#endif

#include "compositor.h"

struct x11_backend *
x11_backend_create(struct weston_compositor *c,
		   int fullscreen,
		   int no_input,
		   int use_pixman);

struct x11_output *
x11_backend_create_output(struct x11_backend *c, int x, int y,
			     int width, int height, int fullscreen,
			     int no_input, char *configured_name,
			     uint32_t transform, int32_t scale);

#ifdef  __cplusplus
}
#endif

#endif
