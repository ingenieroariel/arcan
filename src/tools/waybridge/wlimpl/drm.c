/*
 * Copyright © 2011 Kristian Høgsberg
 * Copyright © 2011 Benjamin Franzke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Kristian Høgsberg <krh@bitplanet.net>
 *    Benjamin Franzke <benjaminfranzke@googlemail.com>
 */

#ifndef WL_DRM_FORMAT_ENUM
#define WL_DRM_FORMAT_ENUM
enum wl_drm_format {
	WL_DRM_FORMAT_C8 = 0x20203843,
	WL_DRM_FORMAT_RGB332 = 0x38424752,
	WL_DRM_FORMAT_BGR233 = 0x38524742,
	WL_DRM_FORMAT_XRGB4444 = 0x32315258,
	WL_DRM_FORMAT_XBGR4444 = 0x32314258,
	WL_DRM_FORMAT_RGBX4444 = 0x32315852,
	WL_DRM_FORMAT_BGRX4444 = 0x32315842,
	WL_DRM_FORMAT_ARGB4444 = 0x32315241,
	WL_DRM_FORMAT_ABGR4444 = 0x32314241,
	WL_DRM_FORMAT_RGBA4444 = 0x32314152,
	WL_DRM_FORMAT_BGRA4444 = 0x32314142,
	WL_DRM_FORMAT_XRGB1555 = 0x35315258,
	WL_DRM_FORMAT_XBGR1555 = 0x35314258,
	WL_DRM_FORMAT_RGBX5551 = 0x35315852,
	WL_DRM_FORMAT_BGRX5551 = 0x35315842,
	WL_DRM_FORMAT_ARGB1555 = 0x35315241,
	WL_DRM_FORMAT_ABGR1555 = 0x35314241,
	WL_DRM_FORMAT_RGBA5551 = 0x35314152,
	WL_DRM_FORMAT_BGRA5551 = 0x35314142,
	WL_DRM_FORMAT_RGB565 = 0x36314752,
	WL_DRM_FORMAT_BGR565 = 0x36314742,
	WL_DRM_FORMAT_RGB888 = 0x34324752,
	WL_DRM_FORMAT_BGR888 = 0x34324742,
	WL_DRM_FORMAT_XRGB8888 = 0x34325258,
	WL_DRM_FORMAT_XBGR8888 = 0x34324258,
	WL_DRM_FORMAT_RGBX8888 = 0x34325852,
	WL_DRM_FORMAT_BGRX8888 = 0x34325842,
	WL_DRM_FORMAT_ARGB8888 = 0x34325241,
	WL_DRM_FORMAT_ABGR8888 = 0x34324241,
	WL_DRM_FORMAT_RGBA8888 = 0x34324152,
	WL_DRM_FORMAT_BGRA8888 = 0x34324142,
	WL_DRM_FORMAT_XRGB2101010 = 0x30335258,
	WL_DRM_FORMAT_XBGR2101010 = 0x30334258,
	WL_DRM_FORMAT_RGBX1010102 = 0x30335852,
	WL_DRM_FORMAT_BGRX1010102 = 0x30335842,
	WL_DRM_FORMAT_ARGB2101010 = 0x30335241,
	WL_DRM_FORMAT_ABGR2101010 = 0x30334241,
	WL_DRM_FORMAT_RGBA1010102 = 0x30334152,
	WL_DRM_FORMAT_BGRA1010102 = 0x30334142,
	WL_DRM_FORMAT_YUYV = 0x56595559,
	WL_DRM_FORMAT_YVYU = 0x55595659,
	WL_DRM_FORMAT_UYVY = 0x59565955,
	WL_DRM_FORMAT_VYUY = 0x59555956,
	WL_DRM_FORMAT_AYUV = 0x56555941,
	WL_DRM_FORMAT_NV12 = 0x3231564e,
	WL_DRM_FORMAT_NV21 = 0x3132564e,
	WL_DRM_FORMAT_NV16 = 0x3631564e,
	WL_DRM_FORMAT_NV61 = 0x3136564e,
	WL_DRM_FORMAT_YUV410 = 0x39565559,
	WL_DRM_FORMAT_YVU410 = 0x39555659,
	WL_DRM_FORMAT_YUV411 = 0x31315559,
	WL_DRM_FORMAT_YVU411 = 0x31315659,
	WL_DRM_FORMAT_YUV420 = 0x32315559,
	WL_DRM_FORMAT_YVU420 = 0x32315659,
	WL_DRM_FORMAT_YUV422 = 0x36315559,
	WL_DRM_FORMAT_YVU422 = 0x36315659,
	WL_DRM_FORMAT_YUV444 = 0x34325559,
	WL_DRM_FORMAT_YVU444 = 0x34325659,
};
#endif /* WL_DRM_FORMAT_ENUM */

struct wl_drm_buffer {
	struct wl_resource *resource;
	struct wl_drm *drm;
	int32_t width, height;
	int fd;
	uint32_t format;
	const void *driver_format;
	int32_t offset[3];
	int32_t stride[3];
	void *driver_buffer;
};

struct wayland_drm_callbacks {
	int (*authenticate)(void *user_data, uint32_t id);
	void (*reference_buffer)(
		void *user_data, uint32_t name, int fd, struct wl_drm_buffer *buffer);
	void (*release_buffer)(void *user_data, struct wl_drm_buffer *buffer);
};

enum { WAYLAND_DRM_PRIME = 0x01 };

#define MIN(x,y) (((x)<(y))?(x):(y))

struct wl_drm {
	struct wl_display *display;
	struct wl_global *wl_drm_global;

	void *user_data;
	char *device_name;
	uint32_t flags;

	struct wayland_drm_callbacks *callbacks;
	struct wl_buffer_interface buffer_interface;
};

static void
destroy_buffer(struct wl_resource *resource)
{
	trace(TRACE_DRM, "");
	struct wl_drm_buffer *buffer = resource->data;
/*
	drm->callbacks->release_buffer(drm->user_data, buffer);
 */
	free(buffer);
}

static void
buffer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	trace(TRACE_DRM, "");
/*
 * struct drm_buffer = return wl_resource_get_user_data(resource);
	struct wl_drm_buffer *buffer;
 */

	wl_resource_destroy(resource);
}

static void create_buffer(struct wl_client *client,
	struct wl_resource *resource, uint32_t id, uint32_t name, int fd,
  int32_t width, int32_t height, uint32_t format,
	int32_t offset0, int32_t stride0, int32_t offset1, int32_t stride1,
	int32_t offset2, int32_t stride2)
{
	struct wl_drm *drm = resource->data;
	struct wl_drm_buffer *buffer;

	buffer = calloc(1, sizeof *buffer);
	if (buffer == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	buffer->drm = drm;
	buffer->width = width;
	buffer->height = height;
	buffer->format = format;
	buffer->fd = fd;
	buffer->offset[0] = offset0;
	buffer->stride[0] = stride0;
	buffer->offset[1] = offset1;
	buffer->stride[1] = stride1;
	buffer->offset[2] = offset2;
	buffer->stride[2] = stride2;

/*
	drm->callbacks->reference_buffer(drm->user_data, name, fd, buffer);
	if (buffer->driver_buffer == NULL){
		wl_resource_post_error(
		resource, WL_DRM_ERROR_INVALID_NAME, "invalid name");
		return;
	}
 */

	buffer->resource = wl_resource_create(client, &wl_buffer_interface, 1, id);
	if (!buffer->resource) {
		wl_resource_post_no_memory(resource);
		free(buffer);
		return;
	}

	wl_resource_set_implementation(
		buffer->resource, (void (**)(void)) &drm->buffer_interface,
		buffer, destroy_buffer
	);
}

static void
drm_create_buffer(struct wl_client *client, struct wl_resource *resource,
	uint32_t id, uint32_t name, int32_t width, int32_t height,
	uint32_t stride, uint32_t format)
{
	switch (format) {
	case WL_DRM_FORMAT_ARGB8888:
	case WL_DRM_FORMAT_XRGB8888:
	case WL_DRM_FORMAT_YUYV:
	case WL_DRM_FORMAT_RGB565:
	break;
	default:
		wl_resource_post_error(resource,
			WL_DRM_ERROR_INVALID_FORMAT, "invalid format");
	return;
	}

	create_buffer(client, resource, id,
		name, -1, width, height, format, 0, stride, 0, 0, 0, 0);
}

static void
drm_create_planar_buffer(struct wl_client *client,
	struct wl_resource *resource,
	uint32_t id, uint32_t name,
	int32_t width, int32_t height, uint32_t format,
	int32_t offset0, int32_t stride0,
	int32_t offset1, int32_t stride1,
	int32_t offset2, int32_t stride2)
{
	trace(TRACE_DRM,
		"%"PRId32",%"PRId32" fmt:%"PRId32, width, height, format);

 	switch (format) {
	case WL_DRM_FORMAT_YUV410:
	case WL_DRM_FORMAT_YUV411:
	case WL_DRM_FORMAT_YUV420:
	case WL_DRM_FORMAT_YUV422:
	case WL_DRM_FORMAT_YUV444:
	case WL_DRM_FORMAT_NV12:
	case WL_DRM_FORMAT_NV16:
	break;
	default:
		wl_resource_post_error(resource,
			WL_DRM_ERROR_INVALID_FORMAT, "invalid format");
	return;
	}

	create_buffer(client, resource, id, name, -1, width, height,
		format, offset0, stride0, offset1, stride1, offset2, stride2);
}

static void
drm_create_prime_buffer(struct wl_client *client,
	struct wl_resource *resource,
	uint32_t id, int fd,
	int32_t width, int32_t height, uint32_t format,
	int32_t offset0, int32_t stride0,
	int32_t offset1, int32_t stride1,
	int32_t offset2, int32_t stride2)
{
	trace(TRACE_DRM,
		"%"PRId32",%"PRId32" fmt:%"PRId32, width, height, format);
	create_buffer(client, resource, id, 0, fd, width, height, format,
		offset0, stride0, offset1, stride1, offset2, stride2);
}

static void
drm_authenticate(struct wl_client *client,
	struct wl_resource *resource, uint32_t id)
{
/*
	struct wl_drm *drm = resource->data;
	if (drm->callbacks->authenticate(drm->user_data, id) < 0)
		wl_resource_post_error(resource,
				       WL_DRM_ERROR_AUTHENTICATE_FAIL,
				       "authenicate failed");
	else
 */
	wl_resource_post_event(resource, WL_DRM_AUTHENTICATED);
}

static const struct wl_drm_interface drm_interface = {
	drm_authenticate,
	drm_create_buffer,
	drm_create_planar_buffer,
	drm_create_prime_buffer
};

static void
bind_drm(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_drm *drm = data;
	struct wl_resource *resource;
	uint32_t capabilities;

	resource = wl_resource_create(client,
		&wl_drm_interface, MIN(version, 2), id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}

	wl_resource_set_implementation(resource, &drm_interface, data, NULL);

	wl_resource_post_event(resource, WL_DRM_DEVICE, drm->device_name);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_ARGB8888);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_XRGB8888);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_RGB565);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_YUV410);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_YUV411);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_YUV420);
 	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_YUV422);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_YUV444);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_NV12);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_NV16);
	wl_resource_post_event(resource, WL_DRM_FORMAT, WL_DRM_FORMAT_YUYV);

/* we only support render nodes, not GEM */
	capabilities = WL_DRM_CAPABILITY_PRIME;

	if (version >= 2)
		wl_resource_post_event(resource, WL_DRM_CAPABILITIES, capabilities);
}

static void wayland_drm_commit(
	struct wl_drm_buffer* buf, struct arcan_shmif_cont* con)
{
	if (buf->width != con->w || buf->height != con->h){
		arcan_shmif_resize(con, buf->width, buf->height);
	}

	arcan_shmif_signalhandle(con,
		SHMIF_SIGVID | SHMIF_SIGBLK_NONE,
			buf->fd, buf->stride[0], buf->format);

/*	close(buf->fd);
	buf->fd = -1; */
}

static struct wl_drm_buffer *
wayland_drm_buffer_get(struct wl_drm *drm, struct wl_resource *resource)
{
	trace(TRACE_DRM, "");
	if (resource == NULL)
		return NULL;

	if (wl_resource_instance_of(resource,
		&wl_buffer_interface, &drm->buffer_interface))
		return wl_resource_get_user_data(resource);
	else
		return NULL;
}

static struct wl_drm* wayland_drm_init(struct wl_display *display,
	char *device_name, struct wayland_drm_callbacks *callbacks, void *user_data,
	uint32_t flags)
{
	struct wl_drm *drm;

	drm = malloc(sizeof *drm);
	if (!drm)
		return NULL;

	drm->display = display;
	drm->device_name = getenv("ARCAN_RENDER_NODE");
	drm->callbacks = callbacks;
	drm->user_data = user_data;
	drm->flags = flags;

	drm->buffer_interface.destroy = buffer_destroy;

	drm->wl_drm_global =
		wl_global_create(display, &wl_drm_interface, 2, drm, bind_drm);

	return drm;
}

static void
wayland_drm_uninit(struct wl_drm *drm)
{
	free(drm->device_name);
	wl_global_destroy(drm->wl_drm_global);
	free(drm);
}

static uint32_t
wayland_drm_buffer_get_format(struct wl_drm_buffer *buffer)
{
	return buffer->format;
}

static void *
wayland_drm_buffer_get_buffer(struct wl_drm_buffer *buffer)
{
	return buffer->driver_buffer;
}
