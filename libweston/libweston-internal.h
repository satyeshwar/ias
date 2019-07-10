/*
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2017, 2018 General Electric Company
 * Copyright © 2012, 2017-2019 Collabora, Ltd.
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

#ifndef LIBWESTON_INTERNAL_H
#define LIBWESTON_INTERNAL_H

/*
 * This is the internal (private) part of libweston. All symbols found here
 * are, and should be only (with a few exceptions) used within the internal
 * parts of libweston.  Notable exception(s) include a few files in tests/ that
 * need access to these functions, screen-share file from compositor/ and those
 * remoting/. Those will require some further fixing as to avoid including this
 * private header.
 *
 * Eventually, these symbols should reside naturally into their own scope. New
 * features should either provide their own (internal) header or use this one.
 */


/* weston_buffer */

void
weston_buffer_send_server_error(struct weston_buffer *buffer,
				const char *msg);
void
weston_buffer_reference(struct weston_buffer_reference *ref,
			struct weston_buffer *buffer);

void
weston_buffer_release_move(struct weston_buffer_release_reference *dest,
			   struct weston_buffer_release_reference *src);

void
weston_buffer_release_reference(struct weston_buffer_release_reference *ref,
				struct weston_buffer_release *buf_release);

/* weston_bindings */
void
weston_binding_destroy(struct weston_binding *binding);

void
weston_binding_list_destroy_all(struct wl_list *list);

/* weston_compositor */

void
touch_calibrator_mode_changed(struct weston_compositor *compositor);

int
noop_renderer_init(struct weston_compositor *ec);

void
weston_compositor_add_head(struct weston_compositor *compositor,
			   struct weston_head *head);
void
weston_compositor_add_pending_output(struct weston_output *output,
				     struct weston_compositor *compositor);
struct weston_binding *
weston_compositor_add_debug_binding(struct weston_compositor *compositor,
				    uint32_t key,
				    weston_key_binding_handler_t binding,
				    void *data);
bool
weston_compositor_import_dmabuf(struct weston_compositor *compositor,
				struct linux_dmabuf_buffer *buffer);
void
weston_compositor_offscreen(struct weston_compositor *compositor);

char *
weston_compositor_print_scene_graph(struct weston_compositor *ec);

void
weston_compositor_read_presentation_clock(
			const struct weston_compositor *compositor,
			struct timespec *ts);

int
weston_compositor_run_axis_binding(struct weston_compositor *compositor,
				   struct weston_pointer *pointer,
				   const struct timespec *time,
				   struct weston_pointer_axis_event *event);
void
weston_compositor_run_button_binding(struct weston_compositor *compositor,
				     struct weston_pointer *pointer,
				     const struct timespec *time,
				     uint32_t button,
				     enum wl_pointer_button_state value);
int
weston_compositor_run_debug_binding(struct weston_compositor *compositor,
				    struct weston_keyboard *keyboard,
				    const struct timespec *time,
				    uint32_t key,
				    enum wl_keyboard_key_state state);
void
weston_compositor_run_key_binding(struct weston_compositor *compositor,
				  struct weston_keyboard *keyboard,
				  const struct timespec *time,
				  uint32_t key,
				  enum wl_keyboard_key_state state);
void
weston_compositor_run_modifier_binding(struct weston_compositor *compositor,
				       struct weston_keyboard *keyboard,
				       enum weston_keyboard_modifier modifier,
				       enum wl_keyboard_key_state state);
void
weston_compositor_run_touch_binding(struct weston_compositor *compositor,
				    struct weston_touch *touch,
				    const struct timespec *time,
				    int touch_type);
void
weston_compositor_stack_plane(struct weston_compositor *ec,
			      struct weston_plane *plane,
			      struct weston_plane *above);
void
weston_compositor_set_touch_mode_normal(struct weston_compositor *compositor);

void
weston_compositor_set_touch_mode_calib(struct weston_compositor *compositor);

int
weston_compositor_set_presentation_clock(struct weston_compositor *compositor,
					 clockid_t clk_id);
int
weston_compositor_set_presentation_clock_software(
					struct weston_compositor *compositor);
void
weston_compositor_shutdown(struct weston_compositor *ec);

void
weston_compositor_xkb_destroy(struct weston_compositor *ec);

int
weston_input_init(struct weston_compositor *compositor);

/* weston_plane */

void
weston_plane_init(struct weston_plane *plane,
			struct weston_compositor *ec,
			int32_t x, int32_t y);
void
weston_plane_release(struct weston_plane *plane);
#endif