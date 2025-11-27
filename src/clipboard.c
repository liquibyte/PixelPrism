/* clipboard.c - X11 Clipboard Management Implementation
 *
 * Provides GTK-like API for X11 CLIPBOARD and PRIMARY selections.
 * Handles both becoming selection owner (copy) and requesting data (paste).
 *
 * X11 selection protocol:
 * 1. Copy: Claim ownership via XSetSelectionOwner
 * 2. Paste: Request data via XConvertSelection, wait for SelectionNotify
 * 3. Respond: Handle SelectionRequest from other apps wanting our data
 * 4. Lost ownership: Handle SelectionClear when another app claims selection
 *
 * Supports both CLIPBOARD (Ctrl+C/V) and PRIMARY (select-to-copy) selections.
 *
 * Internal design notes:
 * - Context tracks owned data separately for CLIPBOARD and PRIMARY.
 * - Pending paste requests are queued and matched via property atom + window.
 * - UTF8_STRING targets are preferred; legacy TEXT falls back when required.
 */

#include "clipboard.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <X11/Xatom.h>

/* Maximum number of simultaneous paste requests */
#define MAX_PENDING_REQUESTS 8

/* Property name for data transfer */
#define CLIPBOARD_PROPERTY_ATOM_NAME "GENERIC_CLIPBOARD"

/* Data for one owned selection (CLIPBOARD or PRIMARY) */
typedef struct {
	char *text; // Owned text data (malloc'd)
	Window owner_window; // Window that owns this selection
} ClipboardData;

/* Pending paste request */
typedef struct {
	Window window;
	ClipboardCallback callback;
	void *user_data;
	SelectionType type;
	Atom property; // Property for data transfer
	int active; // 1 if waiting for response
} PendingRequest;

/* Clipboard context */
struct ClipboardContext {
	Display *dpy;

	// X11 Atoms (cached)
	Atom clipboard_atom; // CLIPBOARD
	Atom utf8_atom; // UTF8_STRING
	Atom text_atom; // TEXT
	Atom targets_atom; // TARGETS
	Atom incr_atom; // INCR
	Atom property_atom; // Property for transfers

	// Data we own
	ClipboardData clipboard_data;
	ClipboardData primary_data;

	// Pending paste requests
	PendingRequest requests[MAX_PENDING_REQUESTS];
};

/* Forward declarations */
static void handle_selection_request(ClipboardContext *ctx, XSelectionRequestEvent *req);
static void handle_selection_notify(ClipboardContext *ctx, XSelectionEvent *sev);
static void handle_selection_clear(ClipboardContext *ctx, XSelectionClearEvent *cev);
static void send_selection_notify(Display *dpy, XSelectionRequestEvent *req, Atom property);
static ClipboardData *get_data_for_selection(ClipboardContext *ctx, Atom selection);
static PendingRequest *find_request(ClipboardContext *ctx, Window win, Atom property);

/* ========== PUBLIC API IMPLEMENTATION ========== */

ClipboardContext *clipboard_create(Display *dpy) {
	if (!dpy) {
		return NULL;
	}
	ClipboardContext *ctx = (ClipboardContext *)calloc(1, sizeof(ClipboardContext));
	if (!ctx) {
		return NULL;
	}
	ctx->dpy = dpy;

	// Initialize atoms
	ctx->clipboard_atom = XInternAtom(dpy, "CLIPBOARD", False);
	ctx->utf8_atom = XInternAtom(dpy, "UTF8_STRING", False);
	ctx->text_atom = XInternAtom(dpy, "TEXT", False);
	ctx->targets_atom = XInternAtom(dpy, "TARGETS", False);
	ctx->incr_atom = XInternAtom(dpy, "INCR", False);
	ctx->property_atom = XInternAtom(dpy, CLIPBOARD_PROPERTY_ATOM_NAME, False);

	return ctx;
}

void clipboard_destroy(ClipboardContext *ctx) {
	if (!ctx) {
		return;
	}
	// Free owned data
	free(ctx->clipboard_data.text);
	free(ctx->primary_data.text);
	// Cancel pending requests
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (ctx->requests[i].active && ctx->requests[i].callback) {
			ctx->requests[i].callback(NULL, ctx->requests[i].user_data);
		}
	}
	free(ctx);
}

void clipboard_set_text(ClipboardContext *ctx, Window win, const char *text, SelectionType type) {
	if (!ctx) {
		return;
	}
	Atom selection = (type == SELECTION_CLIPBOARD) ? ctx->clipboard_atom : XA_PRIMARY;
	ClipboardData *data = get_data_for_selection(ctx, selection);
	if (!data) {
		return;
	}
	// Free old text
	free(data->text);
	data->text = NULL;
	data->owner_window = None;
	// Set new text
	if (text) {
		data->text = strdup(text);
		data->owner_window = win;
		XSetSelectionOwner(ctx->dpy, selection, win, CurrentTime);
	}
	else {
		// Clearing - give up ownership
		XSetSelectionOwner(ctx->dpy, selection, None, CurrentTime);
	}
}

void clipboard_request_text(ClipboardContext *ctx, Window win, ClipboardCallback callback, void *user_data, SelectionType type) {
	if (!ctx || !callback) {
		return;
	}
	Atom selection = (type == SELECTION_CLIPBOARD) ? ctx->clipboard_atom : XA_PRIMARY;

	// Check if selection has an owner
	Window owner = XGetSelectionOwner(ctx->dpy, selection);
	if (owner == None) {
		// No owner, fail immediately
		callback(NULL, user_data);
		return;
	}
	// If we own it, return our own data immediately
	ClipboardData *data = get_data_for_selection(ctx, selection);
	if (data && owner == data->owner_window && data->text) {
		callback(data->text, user_data);
		return;
	}
	// Find free request slot
	PendingRequest *req = NULL;
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (!ctx->requests[i].active) {
			req = &ctx->requests[i];
			break;
		}
	}
	if (!req) {
		fprintf(stderr, "clipboard: too many pending requests\n");
		callback(NULL, user_data);
		return;
	}
	// Setup request
	req->window = win;
	req->callback = callback;
	req->user_data = user_data;
	req->type = type;
	req->property = ctx->property_atom;
	req->active = 1;

	// Request conversion to UTF8
	XConvertSelection(ctx->dpy, selection, ctx->utf8_atom, req->property, win, CurrentTime);
	XFlush(ctx->dpy);
}

int clipboard_handle_event(ClipboardContext *ctx, XEvent *ev) {
	if (!ctx || !ev) {
		return 0;
	}
	switch (ev->type) {
		case SelectionRequest:
			handle_selection_request(ctx, &ev->xselectionrequest);
			return 1;

		case SelectionNotify:
			handle_selection_notify(ctx, &ev->xselection);
			return 1;

		case SelectionClear:
			handle_selection_clear(ctx, &ev->xselectionclear);
			return 1;
	}
	return 0;
}

/* ========== INTERNAL IMPLEMENTATION ========== */

static ClipboardData *get_data_for_selection(ClipboardContext *ctx, Atom selection) {
	if (selection == ctx->clipboard_atom) {
		return &ctx->clipboard_data;
	}
	else if (selection == XA_PRIMARY) {
		return &ctx->primary_data;
	}
	return NULL;
}

static PendingRequest *find_request(ClipboardContext *ctx, Window win, Atom property) {
	for (int i = 0; i < MAX_PENDING_REQUESTS; i++) {
		if (ctx->requests[i].active &&
		    ctx->requests[i].window == win &&
		    ctx->requests[i].property == property) {
			return &ctx->requests[i];
		}
	}
	return NULL;
}

static void send_selection_notify(Display *dpy, XSelectionRequestEvent *req, Atom property) {
	XSelectionEvent sev = {
		.type = SelectionNotify,
		.display = req->display,
		.requestor = req->requestor,
		.selection = req->selection,
		.target = req->target,
		.property = property,
		.time = req->time
	};

	XSendEvent(dpy, req->requestor, False, NoEventMask, (XEvent *)&sev);
	XFlush(dpy);
}

static void handle_selection_request(ClipboardContext *ctx, XSelectionRequestEvent *req) {
	ClipboardData *data = get_data_for_selection(ctx, req->selection);
	// Check if we own this selection
	if (!data || !data->text || req->requestor == data->owner_window) {
		send_selection_notify(ctx->dpy, req, None);
		return;
	}
	// Handle TARGETS request
	if (req->target == ctx->targets_atom) {
		Atom targets[3] = {
			ctx->targets_atom,
			ctx->utf8_atom,
			XA_STRING
		};
		XChangeProperty(ctx->dpy, req->requestor, req->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)targets, 3);
		send_selection_notify(ctx->dpy, req, req->property);
		return;
	}
	// Handle UTF8_STRING or STRING request
	if (req->target == ctx->utf8_atom || req->target == XA_STRING) {
		XChangeProperty(ctx->dpy, req->requestor, req->property, req->target, 8, PropModeReplace, (unsigned char *)data->text, (int)strlen(data->text));
		send_selection_notify(ctx->dpy, req, req->property);
		return;
	}
	// Unsupported target
	send_selection_notify(ctx->dpy, req, None);
}

static void handle_selection_notify(ClipboardContext *ctx, XSelectionEvent *sev) {
	// Find the pending request
	PendingRequest *req = find_request(ctx, sev->requestor, sev->property);
	if (!req) {
		return; // Not our request
	}
	// Check if request was denied
	if (sev->property == None) {
		req->callback(NULL, req->user_data);
		req->active = 0;
		return;
	}
	// Read the property
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop_data = NULL;
	// First call to get size and type
	if (XGetWindowProperty(ctx->dpy, sev->requestor, sev->property, 0, 0, False, AnyPropertyType, &actual_type, &actual_format, &nitems, &bytes_after, &prop_data) != Success) {
		req->callback(NULL, req->user_data);
		req->active = 0;
		return;
	}
	if (prop_data) {
		XFree(prop_data);
		prop_data = NULL;
	}
	// Check for INCR (not supported)
	if (actual_type == ctx->incr_atom) {
		fprintf(stderr, "clipboard: INCR protocol not supported (data too large)\n");
		XDeleteProperty(ctx->dpy, sev->requestor, sev->property);
		req->callback(NULL, req->user_data);
		req->active = 0;
		return;
	}
	// Read the actual data
	if (XGetWindowProperty(ctx->dpy, sev->requestor, sev->property, 0, (long)bytes_after, False, AnyPropertyType, &actual_type, &actual_format, &nitems, &bytes_after, &prop_data) != Success) {
		req->callback(NULL, req->user_data);
		req->active = 0;
		return;
	}
	// Verify we got text
	if (actual_type == ctx->utf8_atom || actual_type == XA_STRING) {
		req->callback((const char *)prop_data, req->user_data);
	}
	else {
		req->callback(NULL, req->user_data);
	}
	// Cleanup
	if (prop_data) {
		XFree(prop_data);
	}
	XDeleteProperty(ctx->dpy, sev->requestor, sev->property);
	req->active = 0;
}

static void handle_selection_clear(ClipboardContext *ctx, XSelectionClearEvent *cev) {
	ClipboardData *data = get_data_for_selection(ctx, cev->selection);
	if (data) {
		// We lost ownership - clear our data
		free(data->text);
		data->text = NULL;
		data->owner_window = None;
	}
}
