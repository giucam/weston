
#ifndef WESTON_COMPOSITOR_DRM_H
#define WESTON_COMPOSITOR_DRM_H

#ifdef  __cplusplus
extern "C" {
#endif

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "compositor.h"

struct libinput_device;

enum drm_output_config {
	DRM_OUTPUT_CONFIG_INVALID = 0,
	DRM_OUTPUT_CONFIG_OFF,
	DRM_OUTPUT_CONFIG_PREFERRED,
	DRM_OUTPUT_CONFIG_CURRENT,
	DRM_OUTPUT_CONFIG_MODE,
	DRM_OUTPUT_CONFIG_MODELINE
};

struct drm_output_parameters {
	uint32_t format;
	char *seat;
	int scale;
	uint32_t transform;
	struct {
		enum drm_output_config config;
		int width;
		int height;
		drmModeModeInfo modeline;
	} mode;
};

struct drm_backend_parameters {
	int connector;
	int tty;
	int use_pixman;
	const char *seat_id;
	uint32_t format;
	void (*get_output_parameters)(const char *name,
				      struct drm_output_parameters *parameters);
	void (*configure_device)(struct weston_compositor *compositor,
				 struct libinput_device *device);
};

struct drm_backend;

struct drm_backend *
drm_backend_create(struct weston_compositor *c,
		   struct drm_backend_parameters *param);

#ifdef  __cplusplus
}
#endif

#endif