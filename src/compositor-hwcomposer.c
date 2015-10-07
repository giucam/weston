/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2011 Intel Corporation
 * Copyright © 2012 Raspberry Pi Foundation
 * Copyright © 2013 Philip Withnall
 * Copyright © 2015 Jolla Ltd.
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

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fb.h>
#include <linux/input.h>

#include <libudev.h>

#include <assert.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <EGL/egl.h>

#ifdef HAVE_HWCOMPOSER_EGL
// libhybris access to the native hwcomposer window
#include <hwcomposer.h>
#endif

#include "compositor.h"
#include "launcher-util.h"
#include "pixman-renderer.h"
#include "libinput-seat.h"
#include "gl-renderer.h"
#include "presentation_timing-server-protocol.h"

// #define HWC_DEBUG

#ifdef HWC_DEBUG
#define HWC_LOG(...) weston_log("HWC: " __VA_ARGS__)
#else
#define HWC_LOG(...)
#endif

struct hwcomposer_compositor {
	struct weston_compositor base;
	uint32_t prev_state;

	struct udev *udev;
	struct udev_input input;
	struct wl_listener session_listener;
	EGLint format;
	struct hwc *hwc;
};

struct hwcomposer_screeninfo {
	unsigned int x_resolution; /* pixels, visible area */
	unsigned int y_resolution; /* pixels, visible area */
	unsigned int width_mm; /* visible screen width in mm */
	unsigned int height_mm; /* visible screen height in mm */
	unsigned int bits_per_pixel;

	size_t buffer_length; /* length of frame buffer memory in bytes */
	size_t line_length; /* length of a line in bytes */
	char id[16]; /* screen identifier */

	unsigned int refresh_rate; /* mHz */
};

struct hwcomposer_output {
	struct weston_output base;
	struct hwcomposer_compositor *compositor;

	struct weston_mode mode;
	struct wl_event_source *finish_frame_timer;

	/* Frame buffer details. */
	const char *device; /* ownership shared with hwcomposer_parameters */
	struct hwcomposer_screeninfo fb_info;
	void *fb; /* length is fb_info.buffer_length */
	struct hwc_output *hwco;
	int index;
};

struct hwcomposer_parameters {
	int tty;
	char *device;
};

struct hwc {
	struct hwcomposer_compositor *compositor;
	hw_module_t *hwc_module;
	hw_device_t *hwc_device;

	struct hwc_output *(*create_hwc_output)(struct hwcomposer_output *o);
	unsigned int (*refresh_rate)(struct hwcomposer_compositor *c);
	void (*set_dpms)(struct hwcomposer_output *o, enum dpms_enum dpms);
	void (*output_repaint)(struct hwcomposer_output *o, pixman_region32_t *damage);
	void (*assign_planes)(struct hwcomposer_output *o);
};

struct hwc_output {
	EGLNativeWindowType native_window;
};

#ifdef HWC_DEVICE_API_VERSION_0_1
struct hwc0 {
	struct hwc base;
	hwc_composer_device_t *composer_device;
};

struct hwc0_output {
	struct hwc_output base;
	hwc_layer_list_t hwc_layer_list;
};
#endif

#ifdef HWC_DEVICE_API_VERSION_1_0
struct hwc10 {
	struct hwc base;
	hwc_composer_device_1_t *composer_device;
};
#endif

#ifdef HWC_DEVICE_API_VERSION_1_1
struct hwc11 {
	struct hwc base;
	hwc_composer_device_1_t *composer_device;
	hwc_procs_t procs;
};

struct hwc11_output {
	struct hwc_output base;
	bool repainted;
	hwc_display_contents_1_t *egl_surface_list;
	struct weston_plane plane;
	hwc_display_contents_1_t *layer_list;
	struct wl_list layer_buffer_list;
	struct wl_list new_layer_buffer_list;
	bool use_egl;
	struct ANativeWindowBuffer *last_egl_buffer;
};

struct hwc11_buffer_state {
	struct wl_list link;
	struct weston_view *view;
	int layer;
	int x, y, width, height;
	EGLClientBuffer egl_buffer;
	struct weston_buffer_reference buffer;
	int release_fence_fd;
	bool accepted;
};

#endif

typedef EGLBoolean (EGLAPIENTRYP Ptr_eglHybrisAcquireNativeBufferWL)(EGLDisplay dpy, struct wl_resource *wlBuffer, EGLClientBuffer *buffer);
typedef EGLBoolean (EGLAPIENTRYP Ptr_eglHybrisNativeBufferHandle)(EGLDisplay dpy, EGLClientBuffer buffer, void **handle);
typedef EGLBoolean (EGLAPIENTRYP Ptr_eglHybrisReleaseNativeBuffer)(EGLClientBuffer buffer);
static Ptr_eglHybrisAcquireNativeBufferWL eglHybrisAcquireNativeBufferWL;
static Ptr_eglHybrisNativeBufferHandle eglHybrisNativeBufferHandle;
static Ptr_eglHybrisReleaseNativeBuffer eglHybrisReleaseNativeBuffer;

struct gl_renderer_interface *gl_renderer;

static const char default_seat[] = "seat0";

static void
hwcomposer_output_start_repaint_loop(struct weston_output *output)
{
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->compositor, &ts);
	weston_output_finish_frame(output, &ts, PRESENTATION_FEEDBACK_INVALID);
}

static int
finish_frame_handler(void *data)
{
	struct hwcomposer_output *output = data;
	struct timespec ts;

	weston_compositor_read_presentation_clock(output->base.compositor, &ts);
	weston_output_finish_frame(&output->base, &ts, 0);

	return 1;
}

static int
hwcomposer_output_repaint(struct weston_output *base, pixman_region32_t *damage)
{
	struct hwcomposer_output *output = (struct hwcomposer_output *)(base);
	struct hwcomposer_compositor *fbc = output->compositor;
	struct weston_compositor *ec = & fbc->base;

	/* Update the damage region. */
	pixman_region32_subtract(&ec->primary_plane.damage,
				&ec->primary_plane.damage, damage);

	fbc->hwc->output_repaint(output, damage);

	return 0;
}

static int
calculate_refresh_rate(struct fb_var_screeninfo *vinfo)
{
	uint64_t quot;

	/* Calculate monitor refresh rate. Default is 60 Hz. Units are mHz. */
	quot = (vinfo->upper_margin + vinfo->lower_margin + vinfo->yres);
	quot *= (vinfo->left_margin + vinfo->right_margin + vinfo->xres);
	quot *= vinfo->pixclock;

	if (quot > 0) {
		uint64_t refresh_rate;

		refresh_rate = 1000000000000000LLU / quot;
		if (refresh_rate > 200000)
			refresh_rate = 200000; /* cap at 200 Hz */

		return refresh_rate;
	}

	return 60 * 1000; /* default to 60 Hz */
}

static int
hwcomposer_query_screen_info(struct hwcomposer_output *output, int fd,
                        struct hwcomposer_screeninfo *info)
{
	struct fb_var_screeninfo varinfo;
	struct fb_fix_screeninfo fixinfo;
	struct hwc *hwc = output->compositor->hwc;
	unsigned int res;

	/* Probe the device for screen information. */
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fixinfo) < 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &varinfo) < 0) {
		return -1;
	}

	/* Store the pertinent data. */
	info->x_resolution = varinfo.xres;
	info->y_resolution = varinfo.yres;
	info->width_mm = varinfo.width;
	info->height_mm = varinfo.height;
	info->bits_per_pixel = varinfo.bits_per_pixel;

	info->buffer_length = fixinfo.smem_len;
	info->line_length = fixinfo.line_length;
	strncpy(info->id, fixinfo.id, sizeof(info->id) / sizeof(*info->id));

	res = hwc->refresh_rate(output->compositor);
	if (res == 0)
		info->refresh_rate = calculate_refresh_rate(&varinfo);
	else
		info->refresh_rate = res;

	return 1;
}

static int
hwcomposer_set_screen_info(struct hwcomposer_output *output, int fd,
                      struct hwcomposer_screeninfo *info)
{
	struct fb_var_screeninfo varinfo;

	/* Grab the current screen information. */
	if (ioctl(fd, FBIOGET_VSCREENINFO, &varinfo) < 0) {
		return -1;
	}

	/* Update the information. */
	varinfo.xres = info->x_resolution;
	varinfo.yres = info->y_resolution;
	varinfo.width = info->width_mm;
	varinfo.height = info->height_mm;
	varinfo.bits_per_pixel = info->bits_per_pixel;

	/* Try to set up an ARGB (x8r8g8b8) pixel format. */
	varinfo.grayscale = 0;
	varinfo.transp.offset = 24;
	varinfo.transp.length = 0;
	varinfo.transp.msb_right = 0;
	varinfo.red.offset = 16;
	varinfo.red.length = 8;
	varinfo.red.msb_right = 0;
	varinfo.green.offset = 8;
	varinfo.green.length = 8;
	varinfo.green.msb_right = 0;
	varinfo.blue.offset = 0;
	varinfo.blue.length = 8;
	varinfo.blue.msb_right = 0;

	/* Set the device's screen information. */
	if (ioctl(fd, FBIOPUT_VSCREENINFO, &varinfo) < 0) {
		return -1;
	}

	return 1;
}

static void hwcomposer_frame_buffer_destroy(struct hwcomposer_output *output);

/* Returns an FD for the frame buffer device. */
static int
hwcomposer_frame_buffer_open(struct hwcomposer_output *output, const char *fb_dev,
                        struct hwcomposer_screeninfo *screen_info)
{
	int fd = -1;

	weston_log("Opening hwcomposer frame buffer.\n");

	/* Open the frame buffer device. */
	fd = open(fb_dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		weston_log("Failed to open frame buffer device ‘%s’: %s\n",
		           fb_dev, strerror(errno));
		return -1;
	}

	/* Grab the screen info. */
	if (hwcomposer_query_screen_info(output, fd, screen_info) < 0) {
		weston_log("Failed to get frame buffer info: %s\n",
		           strerror(errno));

		close(fd);
		return -1;
	}

	return fd;
}

static void
hwcomposer_frame_buffer_destroy(struct hwcomposer_output *output)
{
	weston_log("Destroying hwcomposer frame buffer.\n");

	if (munmap(output->fb, output->fb_info.buffer_length) < 0)
		weston_log("Failed to munmap frame buffer: %s\n",
		           strerror(errno));

	output->fb = NULL;
}

static void hwcomposer_output_destroy(struct weston_output *base);

static void
hwcomposer_output_set_dpms(struct weston_output *base, enum dpms_enum dpms)
{
	struct hwcomposer_output *output = (struct hwcomposer_output *)base;
	output->compositor->hwc->set_dpms(output, dpms);
}

static void
hwcomposer_assign_planes(struct weston_output *base)
{
	struct hwcomposer_output *out = (struct hwcomposer_output *)base;
	struct hwcomposer_compositor *c =
		(struct hwcomposer_compositor *)base->compositor;

	c->hwc->assign_planes(out);
}

static int
hwcomposer_output_create(struct hwcomposer_compositor *compositor,
                    const char *device)
{
	struct hwcomposer_output *output;
	struct weston_config_section *section;
	int fb_fd;
	struct wl_event_loop *loop;
	uint32_t config_transform;
	char *s;

	weston_log("Creating hwcomposer output.\n");

	output = zalloc(sizeof *output);
	if (output == NULL)
		return -1;

	output->compositor = compositor;
	output->device = device;

	/* Create the frame buffer. */
	fb_fd = hwcomposer_frame_buffer_open(output, device, &output->fb_info);
	if (fb_fd < 0) {
		weston_log("Creating frame buffer failed.\n");
		goto out_free;
	}
	close(fb_fd);

	output->base.start_repaint_loop = hwcomposer_output_start_repaint_loop;
	output->base.repaint = hwcomposer_output_repaint;
	output->base.destroy = hwcomposer_output_destroy;
	output->base.set_dpms = hwcomposer_output_set_dpms;
	if (compositor->hwc->assign_planes)
		output->base.assign_planes = hwcomposer_assign_planes;

	/* only one static mode in list */
	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = output->fb_info.x_resolution;
	output->mode.height = output->fb_info.y_resolution;
	output->mode.refresh = output->fb_info.refresh_rate;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current_mode = &output->mode;
	output->base.subpixel = WL_OUTPUT_SUBPIXEL_UNKNOWN;
	output->base.make = "unknown";
	output->base.model = output->fb_info.id;
	output->base.name = strdup("hwcomposer");

	section = weston_config_get_section(compositor->base.config,
					    "output", "name",
					    output->base.name);
	weston_config_section_get_string(section, "transform", &s, "normal");
	if (weston_parse_transform(s, &config_transform) < 0)
		weston_log("Invalid transform \"%s\" for output %s\n",
			   s, output->base.name);
	free(s);

	weston_output_init(&output->base, &compositor->base,
	                   0, 0, output->fb_info.width_mm,
	                   output->fb_info.height_mm,
	                   config_transform, 1);

	output->index = 0;
	output->hwco = compositor->hwc->create_hwc_output(output);
	if (gl_renderer->output_create(&output->base,
	                               output->hwco->native_window, NULL,
	                               gl_renderer->opaque_attribs,
	                               &compositor->format, 1) < 0) {
		weston_log("gl_renderer_output_create failed.\n");
		goto out_destroy;
	}

	loop = wl_display_get_event_loop(compositor->base.wl_display);
	output->finish_frame_timer =
		wl_event_loop_add_timer(loop, finish_frame_handler, output);

	weston_compositor_add_output(&compositor->base, &output->base);

	weston_log("hwcomposer output %d×%d px\n",
	           output->mode.width, output->mode.height);
	weston_log_continue(STAMP_SPACE "guessing %d Hz and 96 dpi\n",
	                    output->mode.refresh / 1000);

	return 0;

out_destroy:
	weston_output_destroy(&output->base);
	hwcomposer_frame_buffer_destroy(output);
out_free:
	free(output);

	return -1;
}

static void
hwcomposer_output_destroy(struct weston_output *base)
{
	struct hwcomposer_output *output = (struct hwcomposer_output *)base;

	weston_log("Destroying hwcomposer output.\n");

	gl_renderer->output_destroy(base);

	/* Remove the output. */
	weston_output_destroy(&output->base);

	free(output);
}

/* strcmp()-style return values. */
static int
compare_screen_info (const struct hwcomposer_screeninfo *a,
                     const struct hwcomposer_screeninfo *b)
{
	if (a->x_resolution == b->x_resolution &&
	    a->y_resolution == b->y_resolution &&
	    a->width_mm == b->width_mm &&
	    a->height_mm == b->height_mm &&
	    a->bits_per_pixel == b->bits_per_pixel &&
	    a->refresh_rate == b->refresh_rate)
		return 0;

	return 1;
}

static int
hwcomposer_output_reenable(struct hwcomposer_compositor *compositor,
                      struct weston_output *base)
{
	struct hwcomposer_output *output = (struct hwcomposer_output *)base;
	struct hwcomposer_screeninfo new_screen_info;
	int fb_fd;
	const char *device;

	weston_log("Re-enabling hwcomposer output.\n");

	/* Create the frame buffer. */
	fb_fd = hwcomposer_frame_buffer_open(output, output->device,
	                                &new_screen_info);
	if (fb_fd < 0) {
		weston_log("Creating frame buffer failed.\n");
		goto err;
	}

	/* Check whether the frame buffer details have changed since we were
	 * disabled. */
	if (compare_screen_info (&output->fb_info, &new_screen_info) != 0) {
		/* Perform a mode-set to restore the old mode. */
		if (hwcomposer_set_screen_info(output, fb_fd,
		                          &output->fb_info) < 0) {
			weston_log("Failed to restore mode settings. "
			           "Attempting to re-open output anyway.\n");
		}

		close(fb_fd);

		/* Remove and re-add the output so that resources depending on
		 * the frame buffer X/Y resolution (such as the shadow buffer)
		 * are re-initialised. */
		device = output->device;
		hwcomposer_output_destroy(base);
		hwcomposer_output_create(compositor, device);

		return 0;
	}

	return 0;

err:
	return -1;
}

static void
hwcomposer_compositor_destroy(struct weston_compositor *base)
{
	struct hwcomposer_compositor *compositor =
				(struct hwcomposer_compositor *)base;

	udev_input_destroy(&compositor->input);

	/* Destroy the output. */
	weston_compositor_shutdown(&compositor->base);

	/* Chain up. */
	weston_launcher_destroy(compositor->base.launcher);

	free(compositor);
}

static void
session_notify(struct wl_listener *listener, void *data)
{
	struct hwcomposer_compositor *compositor = data;
	struct weston_output *output;

	if (compositor->base.session_active) {
		weston_log("entering VT\n");
		compositor->base.state = compositor->prev_state;

		wl_list_for_each(output, &compositor->base.output_list, link) {
			hwcomposer_output_reenable(compositor, output);
		}

		weston_compositor_damage_all(&compositor->base);

		udev_input_enable(&compositor->input);
	} else {
		weston_log("leaving VT\n");
		udev_input_disable(&compositor->input);

		compositor->prev_state = compositor->base.state;
		weston_compositor_offscreen(&compositor->base);

		/* If we have a repaint scheduled (from the idle handler), make
		 * sure we cancel that so we don't try to pageflip when we're
		 * vt switched away.  The OFFSCREEN state will prevent
		 * further attemps at repainting.  When we switch
		 * back, we schedule a repaint, which will process
		 * pending frame callbacks. */

		wl_list_for_each(output,
				 &compositor->base.output_list, link) {
			output->repaint_needed = 0;
		}
	}
}

static void
hwcomposer_restore(struct weston_compositor *compositor)
{
	weston_launcher_restore(compositor->launcher);
}

static void
switch_vt_binding(struct weston_seat *seat, uint32_t time, uint32_t key, void *data)
{
	struct weston_compositor *compositor = data;

	weston_launcher_activate_vt(compositor->launcher, key - KEY_F1 + 1);
}

#ifdef HWC_DEVICE_API_VERSION_0_1
static struct hwc_output *
hwc0_create_hwc_output(struct hwcomposer_output *o)
{
	struct hwc0_output *hwco;

	hwco = zalloc(sizeof *hwco);
	return &hwco->base;
}

static unsigned int
hwc0_refresh_rate(struct hwcomposer_compositor *c)
{
	struct hwc0 *hwc = (struct hwc0 *)c->hwc;
	int32_t frameTime;
	int ret;

	ret = hwc->composer_device->query(hwc->composer_device,
	                                  HWC_VSYNC_PERIOD,
	                                  &frameTime);
	if (ret != 0 || frameTime == 0)
		return 0;

	return (float)1000000000000 / (float)frameTime;
}

static void
hwc0_output_repaint(struct hwcomposer_output *o, pixman_region32_t *damage)
{
	struct weston_compositor *ec = &o->compositor->base;

	ec->renderer->repaint_output(&o->base, damage);
	finish_frame_handler(o);
}

static void
hwc0_output_set_dpms(struct hwcomposer_output *out, enum dpms_enum dpms)
{

}

static void
create_hwc0(struct hwcomposer_compositor *c, hw_module_t *module, hw_device_t *device)
{
	struct hwc0 *hwc;

	weston_log("hwcomposer version 0.\n");

	hwc = zalloc(sizeof *hwc);
	hwc->base.compositor = c;
	hwc->base.hwc_module = module;
	hwc->base.hwc_device = device;
	hwc->base.create_hwc_output = hwc0_create_hwc_output;
	hwc->base.refresh_rate = hwc0_refresh_rate;
	hwc->base.set_dpms = hwc0_output_set_dpms;
	hwc->base.output_repaint = hwc0_output_repaint;

	hwc->composer_device = (hwc_composer_device_t *)device;

	c->hwc = &hwc->base;
}
#endif

#ifdef HWC_DEVICE_API_VERSION_1_0
static void
create_hwc10(struct hwcomposer_compositor *c, hw_module_t *module, hw_device_t *device)
{
	struct hwc10 *hwc;

	hwc = zalloc(sizeof *hwc);
	hwc->base.compositor = c;
	hwc->base.hwc_module = module;
	hwc->base.hwc_device = device;

	hwc->composer_device = (hwc_composer_device_1_t *)device;

	c->hwc = &hwc->base;
}
#endif

#ifdef HWC_DEVICE_API_VERSION_1_1

static void
hwc11_populate_layer(hwc_layer_1_t *layer, int x, int y, int w, int h, buffer_handle_t handle, int32_t type)
{
    layer->handle = handle;
    layer->hints = 0;
    layer->flags = 0;
    layer->compositionType = type;
    layer->blending = HWC_BLENDING_PREMULT;
    layer->transform = 0;
    layer->acquireFenceFd = -1;
    layer->releaseFenceFd = -1;
#ifdef HWC_DEVICE_API_VERSION_1_2
    layer->planeAlpha = 0xff;
#endif
#ifdef HWC_DEVICE_API_VERSION_1_3
    layer->sourceCropf.left = 0;
    layer->sourceCropf.top = 0;
    layer->sourceCropf.right = w;
    layer->sourceCropf.bottom = h;
#else
    layer->sourceCrop.left = 0;
    layer->sourceCrop.top = 0;
    layer->sourceCrop.right = w;
    layer->sourceCrop.bottom = h;
#endif
    layer->displayFrame.left = x;
    layer->displayFrame.top = y;
    layer->displayFrame.right = x + w;
    layer->displayFrame.bottom = y + h;
    layer->visibleRegionScreen.numRects = 1;
    layer->visibleRegionScreen.rects = &layer->displayFrame;
}

static void
hwc11_update_layer(hwc_layer_1_t *layer, int acqFd, buffer_handle_t handle)
{
    layer->handle = handle;
    layer->acquireFenceFd = acqFd;
    layer->releaseFenceFd = -1;
    layer->hints = 0;
}

static void
hwc11_window_present(void *data, struct ANativeWindow *w, struct ANativeWindowBuffer *b)
{
	struct hwcomposer_output *output = data;
	struct hwc11 *hwc = (struct hwc11 *)output->compositor->hwc;
	struct hwc11_output *hwco = (struct hwc11_output *)output->hwco;
	hwc_composer_device_1_t *device = hwc->composer_device;

	if (hwco->layer_list) {
		int n = hwco->layer_list->numHwLayers - 2;
		hwc11_update_layer(&hwco->layer_list->hwLayers[n],
				   HWCNativeBufferGetFence(b), b->handle);

		HWC_LOG("layers + egl\n");

		assert(device->prepare(device, 1, &hwco->layer_list) == 0);
		assert(device->set(device, 1, &hwco->layer_list) == 0);

		HWCNativeBufferSetFence(b, hwco->layer_list->hwLayers[n].releaseFenceFd);
	} else {
		HWC_LOG("egl only\n");

		hwc11_update_layer(hwco->egl_surface_list->hwLayers, HWCNativeBufferGetFence(b), b->handle);
		hwco->egl_surface_list->retireFenceFd = -1;

		assert(device->prepare(device, 1, &hwco->egl_surface_list) == 0);
		assert(device->set(device, 1, &hwco->egl_surface_list) == 0);

		HWCNativeBufferSetFence(b, hwco->egl_surface_list->hwLayers[0].releaseFenceFd);
	}
	hwco->last_egl_buffer = b;
}

static struct hwc_output *
hwc11_create_hwc_output(struct hwcomposer_output *o)
{
	struct hwc11 *hwc = (struct hwc11 *)o->compositor->hwc;
	hwc_composer_device_1_t *device = hwc->composer_device;
	struct weston_compositor *c = &o->compositor->base;
	struct hwc11_output *hwco;
	hwc_display_contents_1_t *list;
	struct ANativeWindow *w;

	device->eventControl(device, 0, HWC_EVENT_VSYNC, 1);

	list = zalloc(sizeof(hwc_display_contents_1_t) + sizeof(hwc_layer_1_t));
	list->retireFenceFd = -1;
	list->outbuf = 0;
	list->outbufAcquireFenceFd = -1;
	list->flags = HWC_GEOMETRY_CHANGED;
	list->numHwLayers = 1;

	hwc11_populate_layer(&list->hwLayers[0], 0, 0, o->base.width,
	                     o->base.height, 0, HWC_FRAMEBUFFER_TARGET);

	w = HWCNativeWindowCreate(o->base.width, o->base.height,
	                          HAL_PIXEL_FORMAT_RGBA_8888,
	                          hwc11_window_present, o);

	hwco = zalloc(sizeof *hwco);
	hwco->base.native_window = (EGLNativeWindowType)w;
	hwco->egl_surface_list = list;
	weston_plane_init(&hwco->plane, c, 0, 0);
	weston_compositor_stack_plane(c, &hwco->plane, &c->primary_plane);
	wl_list_init(&hwco->layer_buffer_list);
	wl_list_init(&hwco->new_layer_buffer_list);

	return &hwco->base;
}

static unsigned int
hwc11_refresh_rate(struct hwcomposer_compositor *c)
{
	return 0;
}

static void
hwc11_output_repaint(struct hwcomposer_output *o, pixman_region32_t *damage)
{
	struct weston_compositor *c = &o->compositor->base;
	struct hwc11 *hwc = (struct hwc11 *)o->compositor->hwc;
	struct hwc11_output *hwco = (struct hwc11_output *)o->hwco;
	hwc_composer_device_1_t *device = hwc->composer_device;
	hwco->repainted = true;

	HWC_LOG("repaint %d\n",pixman_region32_not_empty(damage));

	/* We render with EGL only if we have something to render, that is
	 * if damage is not empty. Otherwise we use the old EGL buffer */
	if (!hwco->layer_list || (hwco->use_egl &&
	    pixman_region32_not_empty(damage))) {
		c->renderer->repaint_output(&o->base, damage);
	} else {
		HWC_LOG("layers only\n");
		assert(device->set(device, 1, &hwco->layer_list) == 0);
		if (hwco->use_egl) {
			int n = hwco->layer_list->numHwLayers - 2;
			HWCNativeBufferSetFence(hwco->last_egl_buffer,
						hwco->layer_list->hwLayers[n].releaseFenceFd);
		}
	}
}

static void
hwc11_build_layer_list(struct hwcomposer_output *o)
{
	struct hwc11 *hwc = (struct hwc11 *)o->compositor->hwc;
	struct hwc11_output *hwco = (struct hwc11_output *)o->hwco;
	struct hwcomposer_compositor *c = o->compositor;
	uint32_t dc_size, i = 0;
	bool accept = false;
	hwc_display_contents_1_t *dc;

	if (wl_list_empty(&hwco->new_layer_buffer_list)) {
		return;
	}

	int views_count = wl_list_length(&c->base.view_list);
	views_count += 2;
	dc_size = sizeof(hwc_display_contents_1_t) +
	                 views_count * sizeof(hwc_layer_1_t);
	dc = zalloc(dc_size);

	dc->retireFenceFd = -1;
	dc->outbuf = 0;
	dc->outbufAcquireFenceFd = -1;
	dc->flags = HWC_GEOMETRY_CHANGED;

	while (!accept && !wl_list_empty(&hwco->new_layer_buffer_list)) {
		struct hwc11_buffer_state *buf_state;
		i = 0;
		wl_list_for_each_reverse(buf_state, &hwco->new_layer_buffer_list, link) {
			if (!buf_state->accepted)
				continue;

			HWC_LOG("put view in layer list %p %s\n", buf_state->view,
				buf_state->view->surface->role_name);
			void *handle = NULL;
			eglHybrisNativeBufferHandle(EGL_NO_DISPLAY,
						buf_state->egl_buffer, &handle);
			buf_state->layer = i;
			hwc11_populate_layer(&dc->hwLayers[i],
					buf_state->x,
					buf_state->y,
					buf_state->width,
					buf_state->height,
					(buffer_handle_t)handle, HWC_FRAMEBUFFER);
			++i;
		}
		if (hwco->use_egl) {
			HWC_LOG("put egl surface in layer list\n");
			hwc11_populate_layer(&dc->hwLayers[i], 0, 0, o->base.width,
					o->base.height,
					hwco->last_egl_buffer->handle,
					HWC_FRAMEBUFFER);
			++i;
		}

		// Add the dummy fallback HWC_FRAMEBUFFER_TARGET layer. This one has
		// buffer handle 0 as we intend to never render to it and that means
		// 'set' is supposed to ignore it.
		hwc11_populate_layer(&dc->hwLayers[i],
				0, 0, o->base.width, o->base.height,
				0, HWC_FRAMEBUFFER_TARGET);
		dc->numHwLayers = i + 1;

		if (hwc->composer_device->prepare(hwc->composer_device, 1, &dc) != 0) {
			HWC_LOG("prepare failed\n");
			assert(0);
			return;
		}

		accept = true;
		for (i = 0; i < dc->numHwLayers - 1; ++i) {
			if (dc->hwLayers[i].compositionType != HWC_OVERLAY) {
				HWC_LOG("\t - layer %d failed\n", i);
				accept = false;
				break;
			}
		}
		if (!accept) {
			/* Not ok, remove one layer and try again. However, this does
			 * mean that we need to do egl rendering in addition to our
			 * own rendering, so we enable that flag regardless of its own
			 * state. This adds another layer, but we also reduce the
			 * total count by one so we're still good with the memory we
			 * allocated for 'dc'. */
			hwco->use_egl = true;
			wl_list_for_each(buf_state, &hwco->new_layer_buffer_list, link) {
				if (buf_state->accepted) {
					buf_state->accepted = false;
					break;
				}
			}
		}
	}

	HWC_LOG("prepare worked\n");
	hwco->layer_list = dc;

	struct hwc11_buffer_state *buf_new;
	wl_list_for_each(buf_new, &hwco->new_layer_buffer_list, link) {
		// If we're posting the same buffer again, we need to close its
		// release fd and mark it as -1 so we don't send release event back
		// to app after composition...
		struct hwc11_buffer_state *buf_old;
		wl_list_for_each(buf_old, &hwco->layer_buffer_list, link) {
			if (buf_new->egl_buffer == buf_old->egl_buffer) {
				int fd = buf_old->release_fence_fd;
				if (fd != -1) {
					HWC_LOG(" - posting buffer=%p again, closing fd=%d\n",
						buf_old->egl_buffer, fd);
					close(fd);
					buf_old->release_fence_fd = -1;
				}
				weston_buffer_reference(&buf_old->buffer, NULL);
			}
		}
	}
}

static void
hwc11_release_fences(struct hwc11_output *hwco)
{
	struct hwc11_buffer_state *buf_state, *next;

	wl_list_for_each_safe(buf_state, next, &hwco->layer_buffer_list, link) {
		int fd = buf_state->release_fence_fd;
		HWC_LOG("release %p %d -> 0\n",buf_state->egl_buffer,fd);
		if (fd != -1) {
			sync_wait(fd, -1);
			close(fd);
		}
		eglHybrisReleaseNativeBuffer(buf_state->egl_buffer);
		weston_buffer_reference(&buf_state->buffer, NULL);
		free(buf_state);
	}
	wl_list_init(&hwco->layer_buffer_list);
}

static void
hwc11_release_old_layers(struct hwc11_output *hwco)
{
	hwc_layer_1_t *layer;

	hwc11_release_fences(hwco);
	if (hwco->layer_list->retireFenceFd != -1)
		close(hwco->layer_list->retireFenceFd);

	wl_list_insert_list(&hwco->layer_buffer_list,
				&hwco->new_layer_buffer_list);
	wl_list_init(&hwco->new_layer_buffer_list);

	struct hwc11_buffer_state *buf_state;
	wl_list_for_each(buf_state, &hwco->layer_buffer_list, link) {
		if (!buf_state->accepted)
			continue;

		layer = &hwco->layer_list->hwLayers[buf_state->layer];
		buf_state->release_fence_fd = layer->releaseFenceFd;

		if (layer->releaseFenceFd == -1) {
			HWC_LOG("after compo %p %d  %p -> 0\n",
				buf_state,
				layer->releaseFenceFd,
				buf_state->buffer.buffer);
			weston_buffer_reference(&buf_state->buffer,
						NULL);
		}
	}
}

static EGLClientBuffer *
hwc11_aquire_native_buffer(struct weston_view *view)
{
	struct weston_surface *surface = view->surface;
	EGLClientBuffer egl_buffer = NULL;

	if (view->alpha < 0.99) {
		HWC_LOG(" - alpha < 1\n");
		return NULL;
	}
	if (!surface->buffer_ref.buffer) {
		HWC_LOG(" - no buf\n");
		return NULL;
	}
	if (!surface->buffer_ref.buffer->resource) {
		HWC_LOG(" - no res\n");
		return NULL;
	}
	if (wl_shm_buffer_get(surface->buffer_ref.buffer->resource)) {
		HWC_LOG(" - shm\n");
		return NULL;
	}

	if (!eglHybrisAcquireNativeBufferWL(EGL_NO_DISPLAY,
	                                    surface->buffer_ref.buffer->resource,
	                                    &egl_buffer))
		HWC_LOG(" - failed to acquire native buffer"
			"(buffers are probably not allocated server-side)\n");
	return egl_buffer;
}

static void
hwc11_assign_planes(struct hwcomposer_output *o)
{
	struct hwc11_output *hwco = (struct hwc11_output *)o->hwco;
	struct hwcomposer_compositor *c = o->compositor;
	struct weston_view *ev, *next;
	struct weston_plane *primary, *next_plane;
	struct hwc11_buffer_state *buf_state;
	const pixman_box32_t *box;

	HWC_LOG("assign planes for output %p, %d views\n", o,
		wl_list_length(&c->base.view_list));

	if (hwco->layer_list)
		hwc11_release_old_layers(hwco);

	hwco->use_egl = false;
	hwco->layer_list = NULL;
	primary = &c->base.primary_plane;

	wl_list_for_each_reverse_safe(ev, next, &c->base.view_list, link) {
		struct weston_surface *s = ev->surface;

		HWC_LOG("view %p %dx%d alpha: %f\n",ev,s->width,s->height,ev->alpha);
		s->keep_buffer = true;

		/** Skip invisible views so we don't use the precious layer
		 * slots for views that cannot be seen. */
		if (ev->alpha < 0.01)
			continue;

		EGLClientBuffer egl_buffer = NULL;
		/** TODO ###
		 * Currently we always put the egl surface on top of the layer list so
		 * if some lower view used egl don't try to use a layer on top of it.
		 * However, we probably could sandwich the egl surface between two hwc
		 * layers, to be implemented. */
		if (!hwco->use_egl)
			egl_buffer = hwc11_aquire_native_buffer(ev);
		if (egl_buffer) {
			HWC_LOG(" - trying hw composition -> %p\n",
				s->buffer_ref.buffer);

			s->keep_buffer = true;

			buf_state = zalloc(sizeof *buf_state);
			buf_state->egl_buffer = egl_buffer;
			buf_state->view = ev;
			buf_state->release_fence_fd = -1;
			buf_state->accepted = true;

			box = pixman_region32_extents(&ev->transform.boundingbox);
			buf_state->x = box->x1;
			buf_state->y = box->y1;
			buf_state->width = box->x2 - box->x1;
			buf_state->height = box->y2 - box->y1;

			weston_buffer_reference(&buf_state->buffer,
						s->buffer_ref.buffer);
			wl_list_insert(&hwco->new_layer_buffer_list,
				       &buf_state->link);
		} else {
			hwco->use_egl = true;
			weston_view_move_to_plane(ev, primary);
		}

		ev->psf_flags = 0;
	}

	hwco->layer_list = NULL;
	hwc11_build_layer_list(o);

	wl_list_for_each(buf_state, &hwco->new_layer_buffer_list, link) {
		next_plane = primary;
		if (buf_state->accepted) {
			next_plane = &hwco->plane;
		} else {
			weston_buffer_reference(&buf_state->buffer, NULL);
		}
		weston_view_move_to_plane(buf_state->view, next_plane);
	}
}

static void
hwc11_output_set_dpms(struct hwcomposer_output *out, enum dpms_enum dpms)
{
	struct hwc11 *hwc = (struct hwc11 *)out->compositor->hwc;
	int blank = dpms != WESTON_DPMS_ON;
	if (blank)
		hwc->composer_device->eventControl(hwc->composer_device,
						   out->index,
						   HWC_EVENT_VSYNC, 0);

	hwc->composer_device->blank(hwc->composer_device, out->index,
				    blank);

	if (!blank) {
		hwc->composer_device->eventControl(hwc->composer_device,
						   out->index,
						   HWC_EVENT_VSYNC, 1);
		weston_output_schedule_repaint(&out->base);
	}
}

static void
hwc11_callback_vsync(const struct hwc_procs *procs, int display,
		     int64_t timestamp)
{
	struct hwc11 *hwc = container_of(procs, struct hwc11, procs);
	struct weston_output *out;
	struct hwcomposer_output *hwo;
	struct hwc11_output *hwco;

	HWC_LOG("--- vsync ---\n");
	wl_list_for_each(out, &hwc->base.compositor->base.output_list, link) {
		hwo = (struct hwcomposer_output *)out;
		hwco = (struct hwc11_output *)hwo->hwco;
		if (hwo->index != display)
			continue;

		if (hwco->repainted) {
			/* TODO use the timestamp passed here */
			/* The hwcomposer docs say: "vsync() is GUARANTEED TO
			 * NOT CALL BACK into the h/w composer HAL", so use a
			 * timer here instead of directly call finish_frame_handler()
			 * since that may directly trigger another output repaint.
			 * ### Note that we cannot use a 0 delay here, as that disarms
			 * the timer. To be fixed. */
			wl_event_source_timer_update(hwo->finish_frame_timer, 1);
			hwco->repainted = false;
		}
		break;
	}
}

static void
hwc11_callback_invalidate(const struct hwc_procs *procs)
{
	weston_log("invalidate\n");
}

static void
hwc11_callback_hotplug(const struct hwc_procs *procs, int display, int connected)
{
	weston_log("hotplug\n");
}

static void
create_hwc11(struct hwcomposer_compositor *c, hw_module_t *module, hw_device_t *device)
{
	struct hwc11 *hwc;

	hwc = zalloc(sizeof *hwc);
	hwc->base.compositor = c;
	hwc->base.hwc_module = module;
	hwc->base.hwc_device = device;
	hwc->base.create_hwc_output = hwc11_create_hwc_output;
	hwc->base.refresh_rate = hwc11_refresh_rate;
	hwc->base.set_dpms = hwc11_output_set_dpms;
	hwc->base.output_repaint = hwc11_output_repaint;
	hwc->base.assign_planes = hwc11_assign_planes;

	hwc->composer_device = (hwc_composer_device_1_t *)device;
	hwc->procs.vsync = hwc11_callback_vsync;
	hwc->procs.invalidate = hwc11_callback_invalidate;
	hwc->procs.hotplug = hwc11_callback_hotplug;
	hwc->composer_device->registerProcs(hwc->composer_device, &hwc->procs);

	c->hwc = &hwc->base;

	const char *acquireNativeBufferExtension = "EGL_HYBRIS_WL_acquire_native_buffer";
	const char *nativeBuffer2Extensions = "EGL_HYBRIS_native_buffer2";
	const char *extensions = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
	int checked = 0;

	if (strstr(extensions, acquireNativeBufferExtension) != 0) {
		eglHybrisAcquireNativeBufferWL = (Ptr_eglHybrisAcquireNativeBufferWL) eglGetProcAddress("eglHybrisAcquireNativeBufferWL");
		checked |= 0x1;
	} else {
		weston_log("Missing required EGL extension: '%s'\n", acquireNativeBufferExtension);
	}
	if (strstr(extensions, nativeBuffer2Extensions) != 0) {
		eglHybrisNativeBufferHandle = (Ptr_eglHybrisNativeBufferHandle) eglGetProcAddress("eglHybrisNativeBufferHandle");
		eglHybrisReleaseNativeBuffer = (Ptr_eglHybrisReleaseNativeBuffer) eglGetProcAddress("eglHybrisReleaseNativeBuffer");
		checked |= 0x2;
	} else {
		weston_log("Missing required EGL extension: '%s'\n", nativeBuffer2Extensions);
	}

	// If both extensions were found
	if (checked == (0x1 | 0x2)) {
		weston_log("HWC composition of window surfaces is enabled\n");
	} else {
		weston_log("HWC composition of window surfaces is disabled\n");
	}
}
#endif

static struct weston_compositor *
hwcomposer_compositor_create(struct wl_display *display, int *argc, char *argv[],
                        struct weston_config *config,
			struct hwcomposer_parameters *param)
{
	struct hwcomposer_compositor *compositor;
	const char *seat_id = default_seat;
	uint32_t key;
	uint32_t version;
	hw_device_t *hwc_device;
	hw_module_t *hwc_module;

	weston_log("initializing hwcomposer backend\n");

	compositor = zalloc(sizeof *compositor);
	if (compositor == NULL)
		return NULL;

	if (weston_compositor_init(&compositor->base, display, argc, argv,
	                           config) < 0)
		goto out_free;

	if (weston_compositor_set_presentation_clock_software(
							&compositor->base) < 0)
		goto out_compositor;

	compositor->udev = udev_new();
	if (compositor->udev == NULL) {
		weston_log("Failed to initialize udev context.\n");
		goto out_compositor;
	}

	/* Set up the TTY. */
	compositor->session_listener.notify = session_notify;
	wl_signal_add(&compositor->base.session_signal,
		      &compositor->session_listener);
	compositor->base.launcher = weston_launcher_connect(&compositor->base,
							    param->tty, "seat0",
							    false);
	if (!compositor->base.launcher) {
		weston_log("fatal: hwcomposer backend should be run "
			   "using weston-launch binary or as root\n");
		goto out_udev;
	}

	compositor->base.destroy = hwcomposer_compositor_destroy;
	compositor->base.restore = hwcomposer_restore;

	compositor->prev_state = WESTON_COMPOSITOR_ACTIVE;

	for (key = KEY_F1; key < KEY_F9; key++)
		weston_compositor_add_key_binding(&compositor->base, key,
		                                  MODIFIER_CTRL | MODIFIER_ALT,
		                                  switch_vt_binding,
		                                  compositor);

#ifdef HAVE_HWCOMPOSER_EGL
	setenv("EGL_PLATFORM", "hwcomposer", 1);
#else
	setenv("EGL_PLATFORM", "fbdev", 1);
#endif

	// Some implementations insist on having the framebuffer module opened before loading
	// the hardware composer one. Therefor we rely on using the fbdev EGL_PLATFORM
	// here and use eglGetDisplay to initialize it.
	eglGetDisplay(EGL_DEFAULT_DISPLAY);

	// Open hardware composer
	assert(hw_get_module(HWC_HARDWARE_MODULE_ID, (const hw_module_t **)(&hwc_module)) == 0);

	weston_log("== hwcomposer module ==\n");
	weston_log(" * Address: %p\n", hwc_module);
	weston_log(" * Module API Version: %x\n", hwc_module->module_api_version);
	weston_log(" * HAL API Version: %x\n", hwc_module->hal_api_version); /* should be zero */
	weston_log(" * Identifier: %s\n", hwc_module->id);
	weston_log(" * Name: %s\n", hwc_module->name);
	weston_log(" * Author: %s\n", hwc_module->author);
	weston_log("== hwcomposer module ==\n");

	// Open hardware composer device
	assert(hwc_module->methods->open(hwc_module, HWC_HARDWARE_COMPOSER, &hwc_device) == 0);

	version = hwc_device->version;
	if ((version & 0xffff0000) == 0) {
		// Assume header version is always 1
		uint32_t header_version = 1;

		// Legacy version encoding
		version = (version << 16) | header_version;
	}

	weston_log("== hwcomposer device ==\n");
	weston_log(" * Version: %x (interpreted as %x)\n", hwc_device->version, version);
	weston_log(" * Module: %p\n", hwc_device->module);
	weston_log("== hwcomposer device ==\n");

	gl_renderer = weston_load_module("gl-renderer.so",
						"gl_renderer_interface");
	if (!gl_renderer) {
		weston_log("could not load gl renderer\n");
		goto out_launcher;
	}

	compositor->format = HAL_PIXEL_FORMAT_RGBA_8888;
	if (gl_renderer->create(&compositor->base, NO_EGL_PLATFORM,
				EGL_DEFAULT_DISPLAY,
				gl_renderer->opaque_attribs,
				&compositor->format, 1) < 0) {
		weston_log("gl_renderer_create failed.\n");
		goto out_launcher;
	}

#ifdef HWC_DEVICE_API_VERSION_0_1
	// Special-case for old hw adaptations that have the version encoded in
	// legacy format, we have to check hwc_device->version directly, because
	// the constants are actually encoded in the old format
	if ((hwc_device->version == HWC_DEVICE_API_VERSION_0_1) ||
		(hwc_device->version == HWC_DEVICE_API_VERSION_0_2) ||
		(hwc_device->version == HWC_DEVICE_API_VERSION_0_3)) {
		create_hwc0(compositor, hwc_module, hwc_device);
	} else
#endif
	switch (version) {
#ifdef HWC_DEVICE_API_VERSION_0_1
	case HWC_DEVICE_API_VERSION_0_1:
	case HWC_DEVICE_API_VERSION_0_2:
	case HWC_DEVICE_API_VERSION_0_3:
		create_hwc0(compositor, hwc_module, hwc_device);
	break;
#endif
#ifdef HWC_DEVICE_API_VERSION_1_0
	case HWC_DEVICE_API_VERSION_1_0:
		weston_log("hwcomposer version 1.0\n");
		create_hwc10(compositor, hwc_module, hwc_device);
		break;
#endif /* HWC_DEVICE_API_VERSION_1_0 */
#ifdef HWC_DEVICE_API_VERSION_1_1
	case HWC_DEVICE_API_VERSION_1_1:
		weston_log("hwcomposer version 1.1\n");
		create_hwc11(compositor, hwc_module, hwc_device);
// 		return new HwComposerBackend_v11(hwc_module, hwc_device, HWC_NUM_DISPLAY_TYPES);
		break;
#endif /* HWC_DEVICE_API_VERSION_1_1 */
#ifdef HWC_DEVICE_API_VERSION_1_2
	case HWC_DEVICE_API_VERSION_1_2:
		/* hwcomposer 1.2 and beyond have virtual displays however virtual displays are
		only used in hwcomposer 1.2 */
		weston_log("hwcomposer version 1.2\n");
		create_hwc11(compositor, hwc_module, hwc_device);
// 		return new HwComposerBackend_v11(hwc_module, hwc_device, HWC_NUM_DISPLAY_TYPES);
		break;

#endif /* HWC_DEVICE_API_VERSION_1_2 */
#ifdef HWC_DEVICE_API_VERSION_1_3
	case HWC_DEVICE_API_VERSION_1_3:
		/* Do not use virtual displays */
		weston_log("hwcomposer version 1.3\n");
		create_hwc11(compositor, hwc_module, hwc_device);
// 		return new HwComposerBackend_v11(hwc_module, hwc_device, HWC_NUM_PHYSICAL_DISPLAY_TYPES);
			break;
#endif /* HWC_DEVICE_API_VERSION_1_3 */
	default:
		weston_log("Unknown hwcomposer API: 0x%x/0x%x/0x%x\n",
			hwc_module->module_api_version,
			hwc_device->version,
			version);
		goto out_launcher;
	}

	if (hwcomposer_output_create(compositor, param->device) < 0)
		goto out_output;

	if (compositor->base.launcher)
		udev_input_init(&compositor->input, &compositor->base, compositor->udev, seat_id);

	return &compositor->base;

out_output:
	compositor->base.renderer->destroy(&compositor->base);

out_launcher:
	if (compositor->base.launcher)
		weston_launcher_destroy(compositor->base.launcher);

out_udev:
	udev_unref(compositor->udev);

out_compositor:
	weston_compositor_shutdown(&compositor->base);

out_free:
	free(compositor);

	return NULL;
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int *argc, char *argv[],
	     struct weston_config *config)
{
	/* TODO: Ideally, available frame buffers should be enumerated using
	 * udev, rather than passing a device node in as a parameter. */
	struct hwcomposer_parameters param = {
		.tty = 0, /* default to current tty */
		.device = "/dev/fb0", /* default frame buffer */
	};

	const struct weston_option hwcomposer_options[] = {
		{ WESTON_OPTION_INTEGER, "tty", 0, &param.tty },
		{ WESTON_OPTION_STRING, "device", 0, &param.device },
	};

	parse_options(hwcomposer_options, ARRAY_LENGTH(hwcomposer_options), argc, argv);

	return hwcomposer_compositor_create(display, argc, argv, config, &param);
}
