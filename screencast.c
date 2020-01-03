#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <wayland-client.h>
#include "wlr-export-dmabuf-unstable-v1-client-protocol.h"

#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <pipewire/client.h>
#include <pipewire/factory.h>

#include <pipewire/pipewire.h>

#include <libdrm/drm_fourcc.h>

//TODO: stop hardcoding
#define HW_DEVICE "/dev/dri/renderD128"
#define WIDTH 1600
#define HEIGHT 900
#define FRAMERATE 25
#define ALIGN 16

//Disableable logger
//#define logger(...) printf(__VA_ARGS__)
#define logger(...)

struct frame_object {
  int32_t fd;
  uint32_t size;
	uint32_t offset;
  uint32_t stride;
};

struct frame_set {
  struct frame_object ** frame_objects;
  uint32_t num_objects;
	uint32_t format;
	uint32_t width;
	uint32_t height;
  uint64_t tv_sec;
  uint32_t tv_nsec;
};

struct screencast_context {

  // pipewire
  struct pw_main_loop *loop;
	struct spa_source *event;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	uint32_t seq;
	bool stream_state;

	struct spa_video_info_raw info;
  pthread_t pw_thread;

  // wlroots
  struct wl_display *display;
  struct wl_list output_list;
  struct wl_registry *registry;
  struct zwlr_export_dmabuf_manager_v1 *export_manager;
  
  // main frame callback
	struct zwlr_export_dmabuf_frame_v1 *frame_callback;

  // target output
  struct wayland_output *target_output;
  bool with_cursor;
  
  // frame data
  struct zwlr_export_dmabuf_frame_v1 *frame;
  struct frame_set frame_set;

	// frame mutex
	pthread_mutex_t lock;

  // config
  char *hardware_device;

  // if something happens during capture
	int err;
	bool quit;
};

struct wayland_output {
	struct wl_list link;
	uint32_t id;
	struct wl_output *output;
	char *make;
	char *model;
	int width;
	int height;
  float framerate;
};

struct screencast_context ctx;

//
//
//
//### utilities ###
//
//
//

static enum spa_video_format drm_fmt_to_spa_fmt(uint32_t fmt) {
	switch (fmt) {
	case DRM_FORMAT_NV12: return SPA_VIDEO_FORMAT_NV12;
	case DRM_FORMAT_ARGB8888: return SPA_VIDEO_FORMAT_BGRA;
	case DRM_FORMAT_XRGB8888: return SPA_VIDEO_FORMAT_BGRx;
	case DRM_FORMAT_ABGR8888: return SPA_VIDEO_FORMAT_RGBA;
	case DRM_FORMAT_XBGR8888: return SPA_VIDEO_FORMAT_RGBx;
	case DRM_FORMAT_RGBA8888: return SPA_VIDEO_FORMAT_ABGR;
	case DRM_FORMAT_RGBX8888: return SPA_VIDEO_FORMAT_xBGR;
	case DRM_FORMAT_BGRA8888: return SPA_VIDEO_FORMAT_ARGB;
	case DRM_FORMAT_BGRX8888: return SPA_VIDEO_FORMAT_xRGB;
	default: return SPA_VIDEO_FORMAT_UNKNOWN;
	};
}

static char *strdup(const char *src) {
    char *dst = malloc(strlen (src) + 1);  // Space for length plus nul
    if (dst == NULL) return NULL;          // No memory
    strcpy(dst, src);                      // Copy the characters
    return dst;                            // Return the new string
}

static void wlr_frame_free(struct screencast_context *ctx);
static void wlr_register_cb(struct screencast_context *ctx);

//
//
//
//### pipewire ###
///
///
///

/* called on timeout and we should push a new buffer in the queue */
static void pw_on_event(void *data, uint64_t expirations){
  struct screencast_context *ctx = data;
  struct pw_buffer *b;
  struct spa_buffer *buf;
	struct spa_meta_header *h;
	struct spa_data *d;

	if(!ctx->stream_state){
		wlr_frame_free(ctx);
		return ;
	}
  
	logger("pw event fired\n");

  if ((b = pw_stream_dequeue_buffer(ctx->stream)) == NULL) {
		printf("out of buffers\n");
		return;
	}
  
	buf = b->buffer;
	d = buf->datas;
	if ((d[0].data) == NULL)
			return;

	if ((h = spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h)))) {
		h->pts = -1;
		h->flags = 0;
		h->seq = ctx->seq++;
		h->dts_offset = 0;
	}

	
  uint64_t i;
  for(i=0; i<1; i++){//i<ctx->frame_set.num_objects; i++){
    d[i].type = SPA_DATA_DmaBuf;
	  d[i].flags = SPA_DATA_FLAG_READABLE;
	  d[i].fd = ctx->frame_set.frame_objects[i]->fd;
		d[i].mapoffset = 0;
		d[i].maxsize = ctx->frame_set.frame_objects[i]->size;
		d[i].chunk->size = ctx->frame_set.frame_objects[i]->size;
		d[i].chunk->stride = ctx->frame_set.frame_objects[i]->stride;
		d[i].chunk->offset = ctx->frame_set.frame_objects[i]->offset;

		logger("************** \n");
		logger("pointer: %p\n", d[i].data);
		logger("fd: %ld\n", d[i].fd);
		logger("size: %d\n", d[i].maxsize);
		logger("offset: %d\n", d[i].mapoffset);
		logger("stride: %d\n", d[i].chunk->stride);
		logger("flags: %d\n", d[i].flags);
		logger("************** \n");
  }

  pw_stream_queue_buffer(ctx->stream, b);

	wlr_frame_free(ctx);
}

static void pw_on_stream_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state,
				    const char *error){
  struct screencast_context *ctx = data;

  printf("pw stream state changed to %d\n", state);

  switch (state) {
    case PW_STREAM_STATE_PAUSED:
      printf("node id: %d\n", pw_stream_get_node_id(ctx->stream));
			ctx->stream_state = false;
      
      break;
    case PW_STREAM_STATE_STREAMING:
			ctx->stream_state = true; 
      break;
    default:
			ctx->stream_state = false;
      break;
  }

}

static void pw_on_stream_add_buffer(void *data, struct pw_buffer *buffer){
  struct screencast_context *ctx = data;
	struct spa_buffer *buf = buffer->buffer;
	struct spa_data *d;

  logger("pw buffer added\n");

	d = buf->datas;

	d[0].type = SPA_DATA_DmaBuf;
  d[0].flags = SPA_DATA_FLAG_READABLE;
	d[0].fd = ctx->frame_set.frame_objects[0]->fd;
	d[0].mapoffset = 0;
	d[0].maxsize = ctx->frame_set.frame_objects[0]->size;
	
}

static void pw_on_stream_remove_buffer(void *data, struct pw_buffer *buffer){
  struct screencast_context *ctx = data;

  logger("pw buffer removed\n");

  wlr_frame_free(ctx);
}

static void pw_on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param){
	struct screencast_context *ctx = data;
	struct pw_stream *stream = ctx->stream;
	uint8_t params_buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	const struct spa_pod *params[2];

	if (param == NULL || id != SPA_PARAM_Format)
		return;
  
	logger("pw format changed\n");

	spa_format_video_raw_parse(param, &ctx->info);

	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, 32),
		SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size,    SPA_POD_Int(ctx->frame_set.frame_objects[0]->size),
		SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(ctx->frame_set.frame_objects[0]->stride),
		SPA_PARAM_BUFFERS_align,   SPA_POD_Int(ALIGN));

	params[1] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

	pw_stream_update_params(stream, params, 2);
}


static const struct pw_stream_events pw_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = pw_on_stream_state_changed,
	.param_changed = pw_on_stream_param_changed,
	.add_buffer = pw_on_stream_add_buffer,
	.remove_buffer = pw_on_stream_remove_buffer,
};

void *pw_start(void *data){

  struct screencast_context *ctx = data;
	
  const struct spa_pod *params[1];
  uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	
  pw_init(NULL, NULL);

	/* create a main loop */
	ctx->loop = pw_main_loop_new(NULL);

	/* create a simple stream */
	ctx->stream = pw_stream_new_simple(
			pw_main_loop_get_loop(ctx->loop),
			"video-src-alloc",
			pw_properties_new(
				PW_KEY_MEDIA_CLASS, "Video/Source",
				NULL),
			&pw_events,
			ctx);
	ctx->stream_state = false;

	/* make an event to signal frame ready */
	ctx->event = pw_loop_add_event(pw_main_loop_get_loop(ctx->loop), pw_on_event, ctx);
	
	/* build the extra parameter for the connection. Here we make an
	 * EnumFormat parameter which lists the possible formats we can provide.
	 * The server will select a format that matches and informs us about this
	 * in the stream format_changed event.
	 */
	params[0] = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format,    SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
		
		// eventually, we can detect the format provided by WLR and convert it to SPA with the function in utils
		// SPA_POD_CHOICE_ENUM_Id(SPA_VIDEO_FORMAT_NV12, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_ABGR, SPA_VIDEO_FORMAT_xBGR, SPA_VIDEO_FORMAT_ARGB, SPA_VIDEO_FORMAT_xRGB)
		
		// hardcoded width and height
		SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
						&SPA_RECTANGLE(WIDTH, HEIGHT),
						&SPA_RECTANGLE(1, 1),
						&SPA_RECTANGLE(4096, 4096)),

		// hardcoded framerate
		SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(FRAMERATE, 1)));

	/* now connect the stream, we need a direction (input/output),
	 * an optional target node to connect to, some flags and parameters.
	 *
	 * Here we pass PW_STREAM_FLAG_ALLOC_BUFFERS. We should in the
	 * add_buffer callback configure the buffer memory. This should be
	 * fd backed memory (memfd, dma-buf, ...) that can be shared with
	 * the server.  */
	pw_stream_connect(ctx->stream,
			  PW_DIRECTION_OUTPUT,
			  SPA_ID_INVALID,
			  PW_STREAM_FLAG_DRIVER |
			  PW_STREAM_FLAG_ALLOC_BUFFERS,
			  params, 1);

	/* run the loop, this will trigger the callbacks */
	pw_main_loop_run(ctx->loop);

	pw_stream_destroy(ctx->stream);
	pw_main_loop_destroy(ctx->loop);
  return NULL;
}

//
//
//
//### wlroots ###
//
//
//

static void wlr_frame_free(struct screencast_context *ctx) {
	
	for (uint64_t i = 0; i < ctx->frame_set.num_objects; ++i) {
		close(ctx->frame_set.frame_objects[i]->fd);
		free(ctx->frame_set.frame_objects[i]);
	}
	free(ctx->frame_set.frame_objects);

	zwlr_export_dmabuf_frame_v1_destroy(ctx->frame);
	logger("wlr frame_freed\n");
	pthread_mutex_unlock(&ctx->lock);

}

static void wlr_frame_start(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t width, uint32_t height, uint32_t offset_x, uint32_t offset_y,
		uint32_t buffer_flags, uint32_t flags, uint32_t format,
		uint32_t mod_high, uint32_t mod_low, uint32_t num_objects) {
  
  struct screencast_context *ctx = data;
	pthread_mutex_lock(&ctx->lock);
  logger("wlr frame_start\n");

  ctx->frame = frame;
  ctx->frame_set.frame_objects = malloc(sizeof(*ctx->frame_set.frame_objects) * (int)num_objects);
  int i;
  for(i=0; i<(int)num_objects; i++){
    ctx->frame_set.frame_objects[i] = malloc(sizeof(struct frame_object));
  }
  ctx->frame_set.num_objects = num_objects;
	pthread_mutex_unlock(&ctx->lock);

}

static void wlr_frame_object(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t index, int32_t fd, uint32_t size, uint32_t offset,
		uint32_t stride, uint32_t plane_index) {
  
  struct screencast_context *ctx = data;
	pthread_mutex_lock(&ctx->lock);
	logger("wlr frame_object\n");


  ctx->frame_set.frame_objects[index]->fd = fd;
  ctx->frame_set.frame_objects[index]->size = size;
	ctx->frame_set.frame_objects[index]->offset = offset;
  ctx->frame_set.frame_objects[index]->stride = stride;
	pthread_mutex_unlock(&ctx->lock);

}

static void wlr_frame_ready(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {

  struct screencast_context *ctx = data;
	pthread_mutex_lock(&ctx->lock);
  logger("wlr frame_ready\n");


  ctx->frame_set.tv_sec = ((((uint64_t)tv_sec_hi) << 32) | tv_sec_lo);
  ctx->frame_set.tv_nsec = tv_nsec;
  
  if (!ctx->quit && !ctx->err) {
	  pw_loop_signal_event(pw_main_loop_get_loop(ctx->loop), ctx->event);
	wlr_register_cb(ctx);
	}

}

static void wlr_frame_cancel(void *data, struct zwlr_export_dmabuf_frame_v1 *frame,
		uint32_t reason) {

	struct screencast_context *ctx = data;
  logger("wlr frame_cancel\n");

  wlr_frame_free(ctx);
  ctx->frame_set.num_objects = 0;

	if (reason == ZWLR_EXPORT_DMABUF_FRAME_V1_CANCEL_REASON_PERMANENT) {
		printf("Permanent failure, exiting\n");
		ctx->err = true;
	} else {
	wlr_register_cb(ctx);
	}

}

static const struct zwlr_export_dmabuf_frame_v1_listener wlr_frame_listener = {
	.frame = wlr_frame_start,
	.object = wlr_frame_object,
	.ready = wlr_frame_ready,
	.cancel = wlr_frame_cancel,
};

static void wlr_register_cb(struct screencast_context *ctx) {
	ctx->frame_callback = zwlr_export_dmabuf_manager_v1_capture_output(
			ctx->export_manager, ctx->with_cursor, ctx->target_output->output);

	zwlr_export_dmabuf_frame_v1_add_listener(ctx->frame_callback,
			&wlr_frame_listener, ctx);
		logger("wlr callbacks registered\n");
}

static void wlr_output_handle_geometry(void *data, struct wl_output *wl_output,
		int32_t x, int32_t y, int32_t phys_width, int32_t phys_height,
		int32_t subpixel, const char *make, const char *model,
		int32_t transform) {
	struct wayland_output *output = data;
	output->make = strdup(make);
	output->model = strdup(model);
}

static void wlr_output_handle_mode(void *data, struct wl_output *wl_output,
		uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		struct wayland_output *output = data;
		output->width = width;
		output->height = height;
    output->framerate = (float) refresh * 1000;
	}
}

static void wlr_output_handle_done(void* data, struct wl_output *wl_output) {
	/* Nothing to do */
}

static void wlr_output_handle_scale(void* data, struct wl_output *wl_output,
		int32_t factor) {
	/* Nothing to do */
}

static const struct wl_output_listener wlr_output_listener = {
	.geometry = wlr_output_handle_geometry,
	.mode = wlr_output_handle_mode,
	.done = wlr_output_handle_done,
	.scale = wlr_output_handle_scale,
};

static struct wayland_output *wlr_find_output(struct screencast_context *ctx,
		struct wl_output *out, uint32_t id) {
	struct wayland_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->output_list, link) {
		if ((output->output == out) || (output->id == id)) {
			return output;
		}
	}
	return NULL;
}

static void wlr_remove_output(struct wayland_output *out) {
	wl_list_remove(&out->link);
}

static void wlr_registry_handle_add(void *data, struct wl_registry *reg,
		uint32_t id, const char *interface, uint32_t ver) {
	struct screencast_context *ctx = data;

	if (!strcmp(interface, wl_output_interface.name)) {
		struct wayland_output *output = malloc(sizeof(*output));

		output->id = id;
		output->output = wl_registry_bind(reg, id, &wl_output_interface, 1);

		wl_output_add_listener(output->output, &wlr_output_listener, output);
		wl_list_insert(&ctx->output_list, &output->link);
	}

	if (!strcmp(interface, zwlr_export_dmabuf_manager_v1_interface.name)) {
		ctx->export_manager = wl_registry_bind(reg, id,
				&zwlr_export_dmabuf_manager_v1_interface, 1);
	}
}

static void wlr_registry_handle_remove(void *data, struct wl_registry *reg,
		uint32_t id) {
	wlr_remove_output(wlr_find_output((struct screencast_context *)data, NULL, id));
}

static const struct wl_registry_listener wlr_registry_listener = {
	.global = wlr_registry_handle_add,
	.global_remove = wlr_registry_handle_remove,
};

static int wlr_dmabuf_init(struct screencast_context *ctx) {
  //connect to wayland display WAYLAND_DISPLAY or 'wayland-0' if not set
	ctx->display = wl_display_connect(NULL);
	if (!ctx->display) {
		printf("Failed to connect to display!\n");
		return -1;
	}

  //retrieve list of outputs
	wl_list_init(&ctx->output_list);

  //retrieve registry
	ctx->registry = wl_display_get_registry(ctx->display);
	wl_registry_add_listener(ctx->registry, &wlr_registry_listener, ctx);

	wl_display_roundtrip(ctx->display);
	wl_display_dispatch(ctx->display);

  //make sure our wlroots supports export_dmabuf protocol
	if (!ctx->export_manager) {
		printf("Compositor doesn't support %s!\n",
				zwlr_export_dmabuf_manager_v1_interface.name);
		return -1;
	}

	return 0;
}

static void wlr_dmabuf_uninit(struct screencast_context *ctx) {
	struct wayland_output *output, *tmp_o;
	wl_list_for_each_safe(output, tmp_o, &ctx->output_list, link) {
  	wl_list_remove(&output->link);
	}

	if (ctx->export_manager) {
		zwlr_export_dmabuf_manager_v1_destroy(ctx->export_manager);
	}
}

int main(int argc, char *argv[]) {
  int err;
  struct screencast_context ctx = (struct screencast_context){ 0 };

  err = wlr_dmabuf_init(&ctx);
  if (err) {
    goto end;
  }
  
  int output_id;
  struct wayland_output *output, *tmp_o;
	wl_list_for_each_reverse_safe(output, tmp_o, &ctx.output_list, link) {
		printf("Capturable output: %s Model: %s: ID: %i\n",
				output->make, output->model, output->id);
    output_id = output->id;
	}

  output = wlr_find_output(&ctx, NULL, output_id);
	if (!output) {
		printf("Unable to find output with ID %i!\n", output_id);
		return 1;
	}
  
  ctx.target_output = output;
	ctx.with_cursor = true;
	ctx.hardware_device = HW_DEVICE;

  printf("wl_display fd: %d\n", wl_display_get_fd(ctx.display));

	pthread_mutex_init(&ctx.lock, NULL);

 	wlr_register_cb(&ctx);
  
  pthread_create(&ctx.pw_thread, NULL, pw_start, &ctx);

	/* Run capture */
	while (wl_display_dispatch(ctx.display) != -1 && !ctx.err && !ctx.quit);
  
  pthread_join(ctx.pw_thread, NULL);

  return 0;

  end:
    wlr_dmabuf_uninit(&ctx);
    return err;
}