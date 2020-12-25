/*
 * Hopalong - a friendly Wayland compositor
 * Copyright (c) 2020 Ariadne Conill <ariadne@dereferenced.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * This software is provided 'as is' and without any warranty, express or
 * implied.  In no event shall the authors be liable for any damages arising
 * from the use of this software.
 */

#include <stdlib.h>
#include "hopalong-xdg.h"
#include "hopalong-server.h"
#include "hopalong-decoration.h"

static const int resize_edges[] = {
	WLR_EDGE_TOP,
	WLR_EDGE_BOTTOM,
	WLR_EDGE_LEFT,
	WLR_EDGE_RIGHT,
};

bool
hopalong_xdg_view_at(struct hopalong_view *view,
	double lx, double ly, struct wlr_surface **surface, double *sx, double *sy)
{
	double view_sx = lx - view->x;
	double view_sy = ly - view->y;

	double _sx, _sy;
	struct wlr_surface *_surface = NULL;

	if (view->xwayland_surface != NULL)
	{
		_surface = hopalong_view_get_surface(view);

		if (_surface != NULL)
		{
			int w = _surface->current.width;
			int h = _surface->current.height;

			if (lx >= view->x && lx <= view->x + w &&
			    ly >= view->y && ly <= view->y + h)
			{
				*sx = lx - view->x;
				*sy = ly - view->y;
				*surface = _surface;

				return true;
			}
		}
	}

	if (view->xdg_surface != NULL)
	{
		_surface = wlr_xdg_surface_surface_at(view->xdg_surface, view_sx, view_sy, &_sx, &_sy);

		if (_surface != NULL)
		{
			*sx = _sx;
			*sy = _sy;
			*surface = _surface;

			return true;
		}
	}

	/* check for frame areas */
	view->frame_area = -1;
	view->frame_area_edges = WLR_EDGE_NONE;

	for (size_t i = 0; i < HOPALONG_VIEW_FRAME_AREA_COUNT; i++)
	{
		struct wlr_box *box = &view->frame_areas[i];

		if (!box->width && !box->height)
			continue;

		if (lx >= box->x && lx <= box->x + box->width &&
		    ly >= box->y && ly <= box->y + box->height)
		{
			view->frame_area = i;

			if (i < HOPALONG_VIEW_FRAME_AREA_TITLEBAR)
				view->frame_area_edges |= resize_edges[i];
		}
	}

	return view->frame_area != -1;
}

struct hopalong_view *
hopalong_xdg_desktop_view_at(struct hopalong_server *server, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy)
{
	struct hopalong_view *view;

	wl_list_for_each(view, &server->views, link)
	{
		if (hopalong_xdg_view_at(view, lx, ly, surface, sx, sy))
			return view;
	}

	return NULL;
}

static void
hopalong_xdg_surface_map(struct wl_listener *listener, void *data)
{
	return_if_fail(listener != NULL);

	struct hopalong_view *view = wl_container_of(listener, view, map);
	view->mapped = true;

	hopalong_view_focus(view, view->xdg_surface->surface);
}

static void
hopalong_xdg_surface_unmap(struct wl_listener *listener, void *data)
{
	return_if_fail(listener != NULL);

	struct hopalong_view *view = wl_container_of(listener, view, unmap);
	view->mapped = false;
}

static void
hopalong_xdg_surface_destroy(struct wl_listener *listener, void *data)
{
	return_if_fail(listener != NULL);

	struct hopalong_view *view = wl_container_of(listener, view, destroy);
	hopalong_view_destroy(view);
}

static void
hopalong_xdg_toplevel_set_title(struct wl_listener *listener, void *data)
{
	struct hopalong_view *view = wl_container_of(listener, view, set_title);
	view->title_dirty = true;
}

static void
hopalong_xdg_begin_drag(struct hopalong_view *view, enum hopalong_cursor_mode mode, uint32_t edges)
{
	struct hopalong_server *server = view->server;
	return_if_fail(server != NULL);

	struct wlr_surface *focused_surface = server->seat->pointer_state.focused_surface;
	return_if_fail(focused_surface != NULL);

	if (view->xdg_surface->surface != focused_surface)
		return;

	server->grabbed_view = view;
	server->cursor_mode = mode;

	if (mode == HOPALONG_CURSOR_MOVE)
	{
		server->grab_x = server->cursor->x - view->x;
		server->grab_y = server->cursor->y - view->y;
	}
	else if (mode == HOPALONG_CURSOR_RESIZE)
	{
		struct wlr_box geo_box;

		wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);

		double border_x = (view->x + geo_box.x) + ((edges & WLR_EDGE_RIGHT) ? geo_box.width : 0);
		double border_y = (view->y + geo_box.y) + ((edges & WLR_EDGE_BOTTOM) ? geo_box.height : 0);

		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = geo_box;
		server->grab_geobox.x += view->x;
		server->grab_geobox.y += view->y;

		server->resize_edges = edges;
	}
}

static void
hopalong_xdg_toplevel_request_move(struct wl_listener *listener, void *data)
{
	return_if_fail(listener != NULL);

	struct hopalong_view *view = wl_container_of(listener, view, request_move);
	hopalong_xdg_begin_drag(view, HOPALONG_CURSOR_MOVE, 0);
}

static void
hopalong_xdg_toplevel_request_resize(struct wl_listener *listener, void *data)
{
	return_if_fail(listener != NULL);
	return_if_fail(data != NULL);

	struct wlr_xdg_toplevel_resize_event *event = data;
	struct hopalong_view *view = wl_container_of(listener, view, request_resize);
	hopalong_xdg_begin_drag(view, HOPALONG_CURSOR_RESIZE, event->edges);
}

static void
hopalong_xdg_toplevel_minimize(struct hopalong_view *view)
{
	wlr_log(WLR_INFO, "hopalong_xdg_toplevel_minimize: not implemented");
}

static void
hopalong_xdg_toplevel_maximize(struct hopalong_view *view)
{
	wlr_log(WLR_INFO, "hopalong_xdg_toplevel_maximize: not implemented");
}

static void
hopalong_xdg_toplevel_close(struct hopalong_view *view)
{
	struct wlr_xdg_surface *surface = view->xdg_surface;

	if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL || surface->toplevel == NULL)
		return;

	wlr_xdg_toplevel_send_close(surface);
}

static const char *
hopalong_xdg_toplevel_getprop(struct hopalong_view *view, enum hopalong_view_prop prop)
{
	switch (prop)
	{
	case HOPALONG_VIEW_TITLE:
		return view->xdg_surface->toplevel->title;
	case HOPALONG_VIEW_APP_ID:
		return view->xdg_surface->toplevel->app_id;
	}

	return NULL;
}

static struct wlr_surface *
hopalong_xdg_toplevel_get_surface(struct hopalong_view *view)
{
	return view->xdg_surface->surface;
}

static void
hopalong_xdg_toplevel_set_activated(struct hopalong_view *view, bool activated)
{
	wlr_xdg_toplevel_set_activated(view->xdg_surface, activated);
}

static const struct hopalong_view_ops hopalong_xdg_view_ops = {
	.minimize = hopalong_xdg_toplevel_minimize,
	.maximize = hopalong_xdg_toplevel_maximize,
	.close = hopalong_xdg_toplevel_close,
	.getprop = hopalong_xdg_toplevel_getprop,
	.get_surface = hopalong_xdg_toplevel_get_surface,
	.set_activated = hopalong_xdg_toplevel_set_activated,
};

static void
hopalong_xdg_new_surface(struct wl_listener *listener, void *data)
{
	struct hopalong_server *server = wl_container_of(listener, server, new_xdg_surface);
	return_if_fail(server != NULL);

	struct wlr_xdg_surface *xdg_surface = data;
	return_if_fail(xdg_surface != NULL);

	if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL)
		return;

	struct hopalong_view *view = calloc(1, sizeof(*view));
	view->server = server;
	view->xdg_surface = xdg_surface;
	view->ops = &hopalong_xdg_view_ops;

	view->x = view->y = 64;

	/* hook up our xdg_surface events */
	view->map.notify = hopalong_xdg_surface_map;
	wl_signal_add(&xdg_surface->events.map, &view->map);

	view->unmap.notify = hopalong_xdg_surface_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &view->unmap);

	view->destroy.notify = hopalong_xdg_surface_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

	/* hook up our xdg_toplevel events */
	struct wlr_xdg_toplevel *xdg_toplevel = xdg_surface->toplevel;
	return_if_fail(xdg_toplevel != NULL);

	view->request_move.notify = hopalong_xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);

	view->request_resize.notify = hopalong_xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);

	view->title_dirty = true;
	view->set_title.notify = hopalong_xdg_toplevel_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);

	/* add to the list of views */
	wl_list_insert(&server->views, &view->link);
}

/*
 * Sets up resources related to XDG shell support.
 */
void
hopalong_xdg_shell_setup(struct hopalong_server *server)
{
	return_if_fail(server != NULL);

	wl_list_init(&server->views);

	server->xdg_shell = wlr_xdg_shell_create(server->display);
	return_if_fail(server->xdg_shell);

	server->new_xdg_surface.notify = hopalong_xdg_new_surface;
	wl_signal_add(&server->xdg_shell->events.new_surface, &server->new_xdg_surface);

	hopalong_decoration_setup(server);
}

/*
 * Tears down resources related to XDG shell support.
 */
void
hopalong_xdg_shell_teardown(struct hopalong_server *server)
{
	return_if_fail(server != NULL);

	hopalong_decoration_teardown(server);
}
