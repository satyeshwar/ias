/*
 * Copyright © 2017 Pekka Paalanen <pq@iki.fi>
 * Copyright © 2018 Zodiac Inflight Innovations
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

#include <libweston/weston-debug.h>
#include "helpers.h"
#include <libweston/libweston.h>

#include "weston-log-internal.h"

#include "weston-debug-server-protocol.h"

#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

/** Main weston-log context
 *
 * One per weston_compositor. Stores list of scopes created and a list pending
 * subscriptions.
 *
 * A pending subscription is a subscription to a scope which hasn't been
 * created. When the scope is finally created the pending subscription will be
 * removed from the pending subscription list, but not before was added in the
 * scope's subscription list and that of the subscriber list.
 *
 * Pending subscriptions only make sense for other types of streams, other than
 * those created by weston-debug protocol. In the case of the weston-debug
 * protocol, the subscription processes is done automatically whenever a client
 * connects and subscribes to a scope which was previously advertised by the
 * compositor.
 *
 * @internal
 */
struct weston_log_context {
	struct wl_listener compositor_destroy_listener;
	struct wl_global *global;
	struct wl_list scope_list; /**< weston_log_scope::compositor_link */
	struct wl_list pending_subscription_list; /**< weston_log_subscription::source_link */
};

/** weston-log message scope
 *
 * This is used for scoping logging/debugging messages. Clients can subscribe
 * to only the scopes they are interested in. A scope is identified by its name
 * (also referred to as debug stream name).
 */
struct weston_log_scope {
	char *name;
	char *desc;
	weston_log_scope_cb begin_cb;
	void *user_data;
	struct wl_list compositor_link;
	struct wl_list subscription_list;  /**< weston_log_subscription::source_link */
};

/** Ties a subscriber to a scope
 *
 * A subscription is created each time we'd want to subscribe to a scope. From
 * the stream type we can retrieve the subscriber and from the subscriber we
 * reach each of the streams callbacks. See also weston_log_subscriber object.
 *
 * When a subscription has been created we store it in the scope's subscription
 * list and in the subscriber's subscription list. The subscription might be a
 * pending subscription until the scope for which there's was a subscribe has
 * been created. The scope creation will take of looking through the pending
 * subscription list.
 *
 * A subscription can reached from a subscriber subscription list by using the
 * streams base class.
 *
 * @internal
 */
struct weston_log_subscription {
	struct weston_log_subscriber *owner;
	struct wl_list owner_link;      /**< weston_log_subscriber::subscription_list */

	char *scope_name;
	struct weston_log_scope *source;
	struct wl_list source_link;     /**< weston_log_scope::subscription_list  or
					  weston_log_context::pending_subscription_list */
};


/** A debug stream created by a client
 *
 * A client provides a file descriptor for the server to write debug
 * messages into. A weston_debug_stream is associated to one
 * weston_log_scope via the scope name, and the scope provides the messages.
 * There can be several streams for the same scope, all streams getting the
 * same messages.
 */
struct weston_debug_stream {
	struct weston_log_subscriber base;
	int fd;				/**< client provided fd */
	struct wl_resource *resource;	/**< weston_debug_stream_v1 object */
};

static struct weston_debug_stream *
to_weston_debug_stream(struct weston_log_subscriber *sub)
{
        return container_of(sub, struct weston_debug_stream, base);
}

/** Creates a new subscription using the subscriber by \c owner.
 *
 * The subscription created is added to the \c owner subscription list.
 * Destroying the subscription using weston_log_subscription_destroy() will
 * remove the link from the subscription list and free storage alloc'ed.
 *
 * @param owner the subscriber owner, must be created before creating a
 * subscription
 * @param scope_name the scope for which to create this subscription
 * @returns a weston_log_subscription object in case of success, or NULL
 * otherwise
 *
 * @internal
 * @sa weston_log_subscription_destroy, weston_log_subscription_remove,
 * weston_log_subscription_add
 */
struct weston_log_subscription *
weston_log_subscription_create(struct weston_log_subscriber *owner,
			       const char *scope_name)
{
	struct weston_log_subscription *sub;
	assert(owner);
	assert(scope_name);

	sub = zalloc(sizeof(*sub));
	if (!sub)
		return NULL;

	sub->owner = owner;
	sub->scope_name = strdup(scope_name);

	wl_list_insert(&sub->owner->subscription_list, &sub->owner_link);
	return sub;
}

/** Destroys the subscription
 *
 * @param sub
 * @internal
 */
void
weston_log_subscription_destroy(struct weston_log_subscription *sub)
{
	if (sub->owner)
		wl_list_remove(&sub->owner_link);
	free(sub->scope_name);
	free(sub);
}

/** Retrieve a subscription by using the subscriber
 *
 * This is useful when trying to find a subscription from the subscriber by
 * having only access to the stream.
 *
 * @param subscriber the subscriber in question
 * @returns a weston_log_subscription object
 *
 * @internal
 */
struct weston_log_subscription *
weston_log_subscriber_get_only_subscription(struct weston_log_subscriber *subscriber)
{
	struct weston_log_subscription *sub;
	/* unlikely, but can happen */
	if (wl_list_length(&subscriber->subscription_list) == 0)
		return NULL;

	assert(wl_list_length(&subscriber->subscription_list) == 1);

	return wl_container_of(subscriber->subscription_list.prev,
			       sub, owner_link);
}

/** Adds the subscription \c sub to the subscription list of the
 * scope.
 *
 * This should used when the scope has been created, and the subscription \c
 * sub has be created before calling this function.
 *
 * @param scope the scope
 * @param sub the subscription, it must be created before, see
 * weston_log_subscription_create()
 *
 * @internal
 */
void
weston_log_subscription_add(struct weston_log_scope *scope,
			    struct weston_log_subscription *sub)
{
	assert(scope);
	assert(sub);
	/* don't allow subscriptions to have a source already! */
	assert(!sub->source);

	sub->source = scope;
	wl_list_insert(&scope->subscription_list, &sub->source_link);
}

/** Removes the subscription from the scope's subscription list
 *
 * @internal
 */
void
weston_log_subscription_remove(struct weston_log_subscription *sub)
{
	assert(sub);
	if (sub->source)
		wl_list_remove(&sub->source_link);
	sub->source = NULL;
}


static struct weston_log_scope *
get_scope(struct weston_log_context *log_ctx, const char *name)
{
	struct weston_log_scope *scope;

	wl_list_for_each(scope, &log_ctx->scope_list, compositor_link)
		if (strcmp(name, scope->name) == 0)
			return scope;

	return NULL;
}

static void
stream_close_unlink(struct weston_debug_stream *stream)
{
	if (stream->fd != -1)
		close(stream->fd);
	stream->fd = -1;
}

static void WL_PRINTF(2, 3)
stream_close_on_failure(struct weston_debug_stream *stream,
			const char *fmt, ...)
{
	char *msg;
	va_list ap;
	int ret;

	stream_close_unlink(stream);

	va_start(ap, fmt);
	ret = vasprintf(&msg, fmt, ap);
	va_end(ap);

	if (ret > 0) {
		weston_debug_stream_v1_send_failure(stream->resource, msg);
		free(msg);
	} else {
		weston_debug_stream_v1_send_failure(stream->resource,
						    "MEMFAIL");
	}
}

/** Write data into a specific debug stream
 *
 * \param sub The subscriber's stream to write into; must not be NULL.
 * \param[in] data Pointer to the data to write.
 * \param len Number of bytes to write.
 *
 * Writes the given data (binary verbatim) into the debug stream.
 * If \c len is zero or negative, the write is silently dropped.
 *
 * Writing is continued until all data has been written or
 * a write fails. If the write fails due to a signal, it is re-tried.
 * Otherwise on failure, the stream is closed and
 * \c weston_debug_stream_v1.failure event is sent to the client.
 *
 * \memberof weston_debug_stream
 */
static void
weston_debug_stream_write(struct weston_log_subscriber *sub,
			  const char *data, size_t len)
{
	ssize_t len_ = len;
	ssize_t ret;
	int e;
	struct weston_debug_stream *stream = to_weston_debug_stream(sub);

	if (stream->fd == -1)
		return;

	while (len_ > 0) {
		ret = write(stream->fd, data, len_);
		e = errno;
		if (ret < 0) {
			if (e == EINTR)
				continue;

			stream_close_on_failure(stream,
					"Error writing %zd bytes: %s (%d)",
					len_, strerror(e), e);
			break;
		}

		len_ -= ret;
		data += ret;
	}
}

/** Close the debug stream and send success event
 *
 * \param sub Subscriber's stream to close.
 *
 * Closes the debug stream and sends \c weston_debug_stream_v1.complete
 * event to the client. This tells the client the debug information dump
 * is complete.
 *
 * \memberof weston_debug_stream
 */
static void
weston_debug_stream_complete(struct weston_log_subscriber *sub)
{
	struct weston_debug_stream *stream = to_weston_debug_stream(sub);

	stream_close_unlink(stream);
	weston_debug_stream_v1_send_complete(stream->resource);
}

static void
weston_debug_stream_to_destroy(struct weston_log_subscriber *sub)
{
	struct weston_debug_stream *stream = to_weston_debug_stream(sub);
	stream_close_on_failure(stream, "debug name removed");
}

static struct weston_debug_stream *
stream_create(struct weston_log_context *log_ctx, const char *name,
	      int32_t streamfd, struct wl_resource *stream_resource)
{
	struct weston_debug_stream *stream;
	struct weston_log_scope *scope;
	struct weston_log_subscription *sub;

	stream = zalloc(sizeof *stream);
	if (!stream)
		return NULL;


	stream->fd = streamfd;
	stream->resource = stream_resource;

	stream->base.write = weston_debug_stream_write;
	stream->base.destroy = weston_debug_stream_to_destroy;
	stream->base.complete = weston_debug_stream_complete;
	wl_list_init(&stream->base.subscription_list);

	sub = weston_log_subscription_create(&stream->base, name);

	scope = get_scope(log_ctx, name);
	if (scope) {
		weston_log_subscription_add(scope, sub);
		if (scope->begin_cb)
			scope->begin_cb(scope, scope->user_data);
	} else {
		stream_close_on_failure(stream,
					"Debug stream name '%s' is unknown.",
					name);
	}

	return stream;
}

static void
stream_destroy(struct wl_resource *stream_resource)
{
	struct weston_debug_stream *stream;
	struct weston_log_subscription *sub = NULL;

	stream = wl_resource_get_user_data(stream_resource);

	if (stream->fd != -1)
		close(stream->fd);

	sub = weston_log_subscriber_get_only_subscription(&stream->base);
	weston_log_subscription_remove(sub);
	weston_log_subscription_destroy(sub);

	free(stream);
}

static void
weston_debug_stream_destroy(struct wl_client *client,
			    struct wl_resource *stream_resource)
{
	wl_resource_destroy(stream_resource);
}

static const struct weston_debug_stream_v1_interface
						weston_debug_stream_impl = {
	weston_debug_stream_destroy
};

static void
weston_debug_destroy(struct wl_client *client,
		     struct wl_resource *global_resource)
{
	wl_resource_destroy(global_resource);
}

static void
weston_debug_subscribe(struct wl_client *client,
		       struct wl_resource *global_resource,
		       const char *name,
		       int32_t streamfd,
		       uint32_t new_stream_id)
{
	struct weston_log_context *log_ctx;
	struct wl_resource *stream_resource;
	uint32_t version;
	struct weston_debug_stream *stream;

	log_ctx = wl_resource_get_user_data(global_resource);
	version = wl_resource_get_version(global_resource);

	stream_resource = wl_resource_create(client,
					&weston_debug_stream_v1_interface,
					version, new_stream_id);
	if (!stream_resource)
		goto fail;

	stream = stream_create(log_ctx, name, streamfd, stream_resource);
	if (!stream)
		goto fail;

	wl_resource_set_implementation(stream_resource,
				       &weston_debug_stream_impl,
				       stream, stream_destroy);
	return;

fail:
	close(streamfd);
	wl_client_post_no_memory(client);
}

static const struct weston_debug_v1_interface weston_debug_impl = {
	weston_debug_destroy,
	weston_debug_subscribe
};

static void
bind_weston_debug(struct wl_client *client,
		   void *data, uint32_t version, uint32_t id)
{
	struct weston_log_context *log_ctx = data;
	struct weston_log_scope *scope;
	struct wl_resource *resource;

	resource = wl_resource_create(client,
				      &weston_debug_v1_interface,
				      version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &weston_debug_impl,
				       log_ctx, NULL);

       wl_list_for_each(scope, &log_ctx->scope_list, compositor_link) {
		weston_debug_v1_send_available(resource, scope->name,
					       scope->desc);
       }
}

/**
 * Connect weston_compositor structure to weston_log_context structure.
 *
 * \param compositor
 * \param log_ctx
 * \return 0 on success, -1 on failure
 *
 * Sets weston_compositor::weston_log_ctx.
 */
int
weston_log_ctx_compositor_setup(struct weston_compositor *compositor,
			      struct weston_log_context *log_ctx)
{
	assert(!compositor->weston_log_ctx);
	assert(log_ctx);

	compositor->weston_log_ctx = log_ctx;
	return 0;
}

/** Creates  weston_log_context structure
 *
 * \return NULL in case of failure, or a weston_log_context object in case of
 * success
 *
 * weston_log_context is a singleton for each weston_compositor.
 *
 */
WL_EXPORT struct weston_log_context *
weston_log_ctx_compositor_create(void)
{
	struct weston_log_context *log_ctx;

	log_ctx = zalloc(sizeof *log_ctx);
	if (!log_ctx)
		return NULL;

	wl_list_init(&log_ctx->scope_list);

	return log_ctx;
}

/** Destroy weston_log_context structure
 *
 * \param compositor The libweston compositor whose weston-debug to tear down.
 *
 * Clears weston_compositor::weston_log_ctx.
 *
 */
WL_EXPORT void
weston_log_ctx_compositor_destroy(struct weston_compositor *compositor)
{
	struct weston_log_context *log_ctx = compositor->weston_log_ctx;
	struct weston_log_scope *scope;

	if (log_ctx->global)
		wl_global_destroy(log_ctx->global);

	wl_list_for_each(scope, &log_ctx->scope_list, compositor_link)
		weston_log("Internal warning: debug scope '%s' has not been destroyed.\n",
			   scope->name);

	/* Remove head to not crash if scope removed later. */
	wl_list_remove(&log_ctx->scope_list);

	free(log_ctx);

	compositor->weston_log_ctx = NULL;
}

/** Enable weston-debug protocol extension
 *
 * \param compositor The libweston compositor where to enable.
 *
 * This enables the weston_debug_v1 Wayland protocol extension which any client
 * can use to get debug messages from the compositor.
 *
 * WARNING: This feature should not be used in production. If a client
 * provides a file descriptor that blocks writes, it will block the whole
 * compositor indefinitely.
 *
 * There is no control on which client is allowed to subscribe to debug
 * messages. Any and all clients are allowed.
 *
 * The debug extension is disabled by default, and once enabled, cannot be
 * disabled again.
 */
WL_EXPORT void
weston_compositor_enable_debug_protocol(struct weston_compositor *compositor)
{
	struct weston_log_context *log_ctx = compositor->weston_log_ctx;
	assert(log_ctx);
	if (log_ctx->global)
		return;

	log_ctx->global = wl_global_create(compositor->wl_display,
				       &weston_debug_v1_interface, 1,
				       log_ctx, bind_weston_debug);
	if (!log_ctx->global)
		return;

	weston_log("WARNING: debug protocol has been enabled. "
		   "This is a potential denial-of-service attack vector and "
		   "information leak.\n");
}

/** Determine if the debug protocol has been enabled
 *
 * \param wc The libweston compositor to verify if debug protocol has been enabled
 */
WL_EXPORT bool
weston_compositor_is_debug_protocol_enabled(struct weston_compositor *wc)
{
	return wc->weston_log_ctx->global != NULL;
}

/** Register a new debug stream name, creating a log scope
 *
 * \param log_ctx The weston_log_context where to add.
 * \param name The debug stream/scope name; must not be NULL.
 * \param description The debug scope description for humans; must not be NULL.
 * \param begin_cb Optional callback when a client subscribes to this scope.
 * \param user_data Optional user data pointer for the callback.
 * \return A valid pointer on success, NULL on failure.
 *
 * This function is used to create a debug scope. All debug message printing
 * happens for a scope, which allows clients to subscribe to the kind of
 * debug messages they want by \c name.
 *
 * \c name must be unique in the \c weston_compositor instance. \c name and
 * \c description must both be provided. The description is printed when a
 * client asks for a list of supported debug scopes.
 *
 * \c begin_cb, if not NULL, is called when a client subscribes to the
 * debug scope creating a debug stream. This is for debug scopes that need
 * to print messages as a response to a client appearing, e.g. printing a
 * list of windows on demand or a static preamble. The argument \c user_data
 * is passed in to the callback and is otherwise unused.
 *
 * For one-shot debug streams, \c begin_cb should finally call
 * weston_debug_stream_complete() to close the stream and tell the client
 * the printing is complete. Otherwise the client expects more to be written
 * to its file descriptor.
 *
 * The debug scope must be destroyed before destroying the
 * \c weston_compositor.
 *
 * \memberof weston_log_scope
 * \sa weston_debug_stream, weston_log_scope_cb
 */
WL_EXPORT struct weston_log_scope *
weston_compositor_add_log_scope(struct weston_log_context *log_ctx,
				const char *name,
				const char *description,
				weston_log_scope_cb begin_cb,
				void *user_data)
{
	struct weston_log_scope *scope;

	if (!name || !description) {
		weston_log("Error: cannot add a debug scope without name or description.\n");
		return NULL;
	}

	if (!log_ctx) {
		weston_log("Error: cannot add debug scope '%s', infra not initialized.\n",
			   name);
		return NULL;
	}

	if (get_scope(log_ctx, name)){
		weston_log("Error: debug scope named '%s' is already registered.\n",
			   name);
		return NULL;
	}

	scope = zalloc(sizeof *scope);
	if (!scope) {
		weston_log("Error adding debug scope '%s': out of memory.\n",
			   name);
		return NULL;
	}

	scope->name = strdup(name);
	scope->desc = strdup(description);
	scope->begin_cb = begin_cb;
	scope->user_data = user_data;
	wl_list_init(&scope->subscription_list);

	if (!scope->name || !scope->desc) {
		weston_log("Error adding debug scope '%s': out of memory.\n",
			   name);
		free(scope->name);
		free(scope->desc);
		free(scope);
		return NULL;
	}

	wl_list_insert(log_ctx->scope_list.prev, &scope->compositor_link);

	return scope;
}

/** Destroy a log scope
 *
 * \param scope The log scope to destroy; may be NULL.
 *
 * Destroys the log scope, closing all open streams subscribed to it and
 * sending them each a \c weston_debug_stream_v1.failure event.
 *
 * \memberof weston_log_scope
 */
WL_EXPORT void
weston_compositor_log_scope_destroy(struct weston_log_scope *scope)
{
	struct weston_log_subscription *sub, *sub_tmp;

	if (!scope)
		return;

	wl_list_for_each_safe(sub, sub_tmp, &scope->subscription_list, source_link) {
		/* destroy each subscription */
		if (sub->owner->destroy)
			sub->owner->destroy(sub->owner);

		weston_log_subscription_remove(sub);
		weston_log_subscription_destroy(sub);
	}

	wl_list_remove(&scope->compositor_link);
	free(scope->name);
	free(scope->desc);
	free(scope);
}

/** Are there any active subscriptions to the scope?
 *
 * \param scope The log scope to check; may be NULL.
 * \return True if any streams are open for this scope, false otherwise.
 *
 * As printing some debugging messages may be relatively expensive, one
 * can use this function to determine if there is a need to gather the
 * debugging information at all. If this function returns false, all
 * printing for this scope is dropped, so gathering the information is
 * pointless.
 *
 * The return value of this function should not be stored, as new clients
 * may subscribe to the debug scope later.
 *
 * If the given scope is NULL, this function will always return false,
 * making it safe to use in teardown or destroy code, provided the
 * scope is initialized to NULL before creation and set to NULL after
 * destruction.
 *
 * \memberof weston_log_scope
 */
WL_EXPORT bool
weston_log_scope_is_enabled(struct weston_log_scope *scope)
{
	if (!scope)
		return false;

	return !wl_list_empty(&scope->subscription_list);
}

WL_EXPORT void
weston_log_scope_complete(struct weston_log_scope *scope)
{
	struct weston_log_subscription *sub;

	if (!scope)
		return;

	wl_list_for_each(sub, &scope->subscription_list, source_link)
		if (sub->owner && sub->owner->complete)
			sub->owner->complete(sub->owner);
}

/** Write log data for a scope
 *
 * \param scope The debug scope to write for; may be NULL, in which case
 *              nothing will be written.
 * \param[in] data Pointer to the data to write.
 * \param len Number of bytes to write.
 *
 * Writes the given data to all subscribed clients' streams.
 *
 * \memberof weston_log_scope
 */
WL_EXPORT void
weston_log_scope_write(struct weston_log_scope *scope,
		       const char *data, size_t len)
{
	struct weston_log_subscription *sub;

	if (!scope)
		return;

	wl_list_for_each(sub, &scope->subscription_list, source_link)
		if (sub->owner && sub->owner->write)
			sub->owner->write(sub->owner, data, len);
}

/** Write a formatted string for a scope (varargs)
 *
 * \param scope The log scope to write for; may be NULL, in which case
 *              nothing will be written.
 * \param fmt Printf-style format string.
 * \param ap Formatting arguments.
 *
 * Writes to formatted string to all subscribed clients' streams.
 *
 * The behavioral details for each stream are the same as for
 * weston_debug_stream_write().
 *
 * \memberof weston_log_scope
 */
WL_EXPORT void
weston_log_scope_vprintf(struct weston_log_scope *scope,
			 const char *fmt, va_list ap)
{
	static const char oom[] = "Out of memory";
	char *str;
	int len;

	if (!weston_log_scope_is_enabled(scope))
		return;

	len = vasprintf(&str, fmt, ap);
	if (len >= 0) {
		weston_log_scope_write(scope, str, len);
		free(str);
	} else {
		weston_log_scope_write(scope, oom, sizeof oom - 1);
	}
}

/** Write a formatted string for a scope
 *
 * \param scope The log scope to write for; may be NULL, in which case
 *              nothing will be written.
 * \param fmt Printf-style format string and arguments.
 *
 * Writes to formatted string to all subscribed clients' streams.
 *
 * The behavioral details for each stream are the same as for
 * weston_debug_stream_write().
 *
 * \memberof weston_log_scope
 */
WL_EXPORT void
weston_log_scope_printf(struct weston_log_scope *scope,
			  const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	weston_log_scope_vprintf(scope, fmt, ap);
	va_end(ap);
}

/** Write debug scope name and current time into string
 *
 * \param[in] scope debug scope; may be NULL
 * \param[out] buf Buffer to store the string.
 * \param len Available size in the buffer in bytes.
 * \return \c buf
 *
 * Reads the current local wall-clock time and formats it into a string.
 * and append the debug scope name to it, if a scope is available.
 * The string is NUL-terminated, even if truncated.
 */
WL_EXPORT char *
weston_log_scope_timestamp(struct weston_log_scope *scope,
			   char *buf, size_t len)
{
	struct timeval tv;
	struct tm *bdt;
	char string[128];
	size_t ret = 0;

	gettimeofday(&tv, NULL);

	bdt = localtime(&tv.tv_sec);
	if (bdt)
		ret = strftime(string, sizeof string,
			       "%Y-%m-%d %H:%M:%S", bdt);

	if (ret > 0) {
		snprintf(buf, len, "[%s.%03ld][%s]", string,
			 tv.tv_usec / 1000,
			 (scope) ? scope->name : "no scope");
	} else {
		snprintf(buf, len, "[?][%s]",
			 (scope) ? scope->name : "no scope");
	}

	return buf;
}
