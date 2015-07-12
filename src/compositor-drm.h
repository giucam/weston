/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2015 Giulio Camuffo
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

#ifndef WESTON_COMPOSITOR_DRM_H
#define WESTON_COMPOSITOR_DRM_H

#ifdef  __cplusplus
extern "C" {
#endif

#include "compositor.h"

struct weston_drm_backend_modeline;

enum weston_drm_backend_output_type {
	WESTON_DRM_BACKEND_OUTPUT_TYPE_INVALID = 0,
	WESTON_DRM_BACKEND_OUTPUT_TYPE_OFF,
	WESTON_DRM_BACKEND_OUTPUT_TYPE_PREFERRED,
	WESTON_DRM_BACKEND_OUTPUT_TYPE_CURRENT,
	WESTON_DRM_BACKEND_OUTPUT_TYPE_MODE,
	WESTON_DRM_BACKEND_OUTPUT_TYPE_MODELINE
};

enum weston_drm_backend_modeline_flags {
	WESTON_DRM_BACKEND_MODELINE_FLAG_PHSYNC = (1 << 0),
	WESTON_DRM_BACKEND_MODELINE_FLAG_NHSYNC = (1 << 1),
	WESTON_DRM_BACKEND_MODELINE_FLAG_PVSYNC = (1 << 2),
	WESTON_DRM_BACKEND_MODELINE_FLAG_NVSYNC = (1 << 3),
};

struct weston_drm_backend_modeline {
	uint32_t flags;
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
};

struct weston_drm_backend_output_config {
	struct weston_backend_output_config base;

	char *format;
	char *seat;
	enum weston_drm_backend_output_type type;
	struct weston_drm_backend_modeline modeline;
};

struct weston_drm_backend_config {
	struct weston_backend_config base;

	int connector;
	int tty;
	bool use_pixman;
	const char *seat_id;
	const char *format;
	bool default_current_mode;
	void (*configure_output)(struct weston_compositor *compositor,
				 const char *name,
				 struct weston_drm_backend_output_config *config);
};

#ifdef  __cplusplus
}
#endif

#endif
