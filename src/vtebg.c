/*
 * Copyright (C) 2003 Red Hat, Inc.
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <config.h>
#include <stdio.h>
#include <string.h>
#include <gtk/gtk.h>
#include <cairo-xlib.h>
#include "debug.h"
#include "marshal.h"
#include "vtebg.h"

#include <glib/gi18n-lib.h>

G_DEFINE_TYPE(VteBg, vte_bg, G_TYPE_OBJECT)

struct VteBgPrivate {
	GList *cache;
};

struct VteBgCacheItem {
	enum VteBgSourceType source_type;
	GdkPixbuf *source_pixbuf;
	char *source_file;

	PangoColor tint_color;
	double saturation;
	cairo_surface_t *surface;
};


static void vte_bg_cache_item_free(struct VteBgCacheItem *item);
static void vte_bg_cache_prune_int(VteBg *bg, gboolean root);
static const cairo_user_data_key_t item_surface_key;


#if 0
static const char *
vte_bg_source_name(enum VteBgSourceType type)
{
	switch (type) {
	case VTE_BG_SOURCE_NONE:
		return "none";
		break;
	case VTE_BG_SOURCE_ROOT:
		return "root";
		break;
	case VTE_BG_SOURCE_PIXBUF:
		return "pixbuf";
		break;
	case VTE_BG_SOURCE_FILE:
		return "file";
		break;
	}
	return "unknown";
}
#endif

#ifndef X_DISPLAY_MISSING

#include <gdk/gdkx.h>

struct VteBgNative {
	GdkDisplay *display;
	GdkWindow *window;
	XID native_window;
	GdkAtom atom;
	Atom native_atom;
};

static struct VteBgNative *
vte_bg_native_new(GdkWindow *window)
{
	struct VteBgNative *pvt;
	pvt = g_slice_new(struct VteBgNative);
	pvt->window = window;
	pvt->native_window = gdk_x11_drawable_get_xid(window);
	pvt->display = gdk_drawable_get_display(GDK_DRAWABLE(window));
	pvt->native_atom = gdk_x11_get_xatom_by_name_for_display(pvt->display, "_XROOTPMAP_ID");
	pvt->atom = gdk_x11_xatom_to_atom_for_display(pvt->display, pvt->native_atom);
	return pvt;
}

static void
_vte_bg_display_sync(VteBg *bg)
{
	gdk_display_sync(bg->native->display);
}

static gboolean
_vte_property_get_pixmaps(GdkWindow *window, GdkAtom atom,
			  GdkAtom *type, int *size,
			  XID **pixmaps)
{
	return gdk_property_get(window, atom, GDK_TARGET_PIXMAP,
				0, INT_MAX - 3,
				FALSE,
				type, NULL, size,
				(guchar**) pixmaps);
}

static cairo_surface_t *
vte_bg_root_surface(VteBg *bg)
{
	GdkPixmap *pixmap;
	GdkAtom prop_type;
	int prop_size;
	Window root;
	XID *pixmaps;
	int x, y;
	unsigned int width, height, border_width, depth;
	cairo_surface_t *surface = NULL;
	Display *display;
	Screen *screen;

	pixmap = NULL;
	pixmaps = NULL;
	gdk_error_trap_push();
	if (!_vte_property_get_pixmaps(bg->native->window, bg->native->atom,
				      &prop_type, &prop_size,
				      &pixmaps))
		goto out;

	if ((prop_type != GDK_TARGET_PIXMAP) ||
	    (prop_size < (int)sizeof(XID) ||
	     (pixmaps == NULL)))
		goto out_pixmaps;
		
	if (!XGetGeometry (GDK_DISPLAY_XDISPLAY (bg->native->display),
			   pixmaps[0], &root,
			   &x, &y, &width, &height, &border_width, &depth))
		goto out_pixmaps;

	display = gdk_x11_display_get_xdisplay (bg->native->display);
	screen = gdk_x11_screen_get_xscreen (bg->screen);
	surface = cairo_xlib_surface_create (display,
					     pixmaps[0],
					     DefaultVisualOfScreen(screen),
					     width, height);

	_VTE_DEBUG_IF(VTE_DEBUG_MISC|VTE_DEBUG_EVENTS) {
		g_printerr("New background image %dx%d\n", width, height);
	}

 out_pixmaps:
	g_free(pixmaps);
 out:
	_vte_bg_display_sync(bg);
	gdk_error_trap_pop();

	return surface;
}

static void
vte_bg_set_root_surface(VteBg *bg, cairo_surface_t *surface)
{
	if (bg->root_surface != NULL) {
		cairo_surface_destroy (bg->root_surface);
	}
	bg->root_surface = surface;
	vte_bg_cache_prune_int (bg, TRUE);
	g_signal_emit_by_name(bg, "root-pixmap-changed");
}


static GdkFilterReturn
vte_bg_root_filter(GdkXEvent *native, GdkEvent *event, gpointer data)
{
	XEvent *xevent = (XEvent*) native;
	VteBg *bg;
	cairo_surface_t *surface;

	switch (xevent->type) {
	case PropertyNotify:
		bg = VTE_BG(data);
		if ((xevent->xproperty.window == bg->native->native_window) &&
		    (xevent->xproperty.atom == bg->native->native_atom)) {
			surface = vte_bg_root_surface(bg);
			vte_bg_set_root_surface(bg, surface);
		}
		break;
	default:
		break;
	}
	return GDK_FILTER_CONTINUE;
}

#else

struct VteBgNative {
	guchar filler;
};
static struct VteBgNative *
vte_bg_native_new(GdkWindow *window)
{
	return NULL;
}
static GdkFilterReturn
vte_bg_root_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
	return GDK_FILTER_CONTINUE;
}
static void
_vte_bg_display_sync(VteBg *bg)
{
}

static GdkPixmap *
vte_bg_root_pixmap(VteBg *bg)
{
	return NULL;
}
#endif


static void
vte_bg_finalize (GObject *obj)
{
	VteBg *bg;

	bg = VTE_BG (obj);

	if (bg->pvt->cache) {
		g_list_foreach (bg->pvt->cache, (GFunc)vte_bg_cache_item_free, NULL);
		g_list_free (bg->pvt->cache);
	}

	G_OBJECT_CLASS(vte_bg_parent_class)->finalize (obj);
}

static void
vte_bg_class_init(VteBgClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);

	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
#ifdef HAVE_DECL_BIND_TEXTDOMAIN_CODESET
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
#endif

	gobject_class->finalize = vte_bg_finalize;

	klass->root_pixmap_changed = g_signal_new("root-pixmap-changed",
						  G_OBJECT_CLASS_TYPE(klass),
						  G_SIGNAL_RUN_LAST,
						  0,
						  NULL,
						  NULL,
                                                  g_cclosure_marshal_VOID__VOID,
						  G_TYPE_NONE, 0);
	g_type_class_add_private(klass, sizeof (struct VteBgPrivate));
}

static void
vte_bg_init(VteBg *bg)
{
	bg->pvt = G_TYPE_INSTANCE_GET_PRIVATE (bg, VTE_TYPE_BG, struct VteBgPrivate);
}

/**
 * vte_bg_get:
 * @screen : A #GdkScreen.
 *
 * Finds the address of the global #VteBg object, creating the object if
 * necessary.
 *
 * Returns: the global #VteBg object
 */
VteBg *
vte_bg_get_for_screen(GdkScreen *screen)
{
	GdkEventMask events;
	GdkWindow   *window;
	VteBg       *bg;

	bg = g_object_get_data(G_OBJECT(screen), "vte-bg");
	if (G_UNLIKELY(bg == NULL)) {
		bg = g_object_new(VTE_TYPE_BG, NULL);
		g_object_set_data_full(G_OBJECT(screen),
				"vte-bg", bg, (GDestroyNotify)g_object_unref);

		/* connect bg to screen */
		bg->screen = screen;
		window = gdk_screen_get_root_window(screen);
		bg->native = vte_bg_native_new(window);
		bg->root_surface = vte_bg_root_surface(bg);
		events = gdk_window_get_events(window);
		events |= GDK_PROPERTY_CHANGE_MASK;
		gdk_window_set_events(window, events);
		gdk_window_add_filter(window, vte_bg_root_filter, bg);
	}

	return bg;
}

static gboolean
vte_bg_colors_equal(const PangoColor *a, const PangoColor *b)
{
	return  (a->red >> 8) == (b->red >> 8) &&
		(a->green >> 8) == (b->green >> 8) &&
		(a->blue >> 8) == (b->blue >> 8);
}

static void
vte_bg_cache_item_free(struct VteBgCacheItem *item)
{
	/* Clean up whatever is left in the structure. */
	if (item->source_pixbuf != NULL) {
		g_object_remove_weak_pointer(G_OBJECT(item->source_pixbuf),
				(gpointer*)(void*)&item->source_pixbuf);
	}
	g_free(item->source_file);

	if (item->surface != NULL)
		cairo_surface_set_user_data (item->surface,
					     &item_surface_key, NULL, NULL);

	g_slice_free(struct VteBgCacheItem, item);
}

static void
vte_bg_cache_prune_int(VteBg *bg, gboolean root)
{
	GList *i, *next;
	for (i = bg->pvt->cache; i != NULL; i = next) {
		struct VteBgCacheItem *item = i->data;
		next = g_list_next (i);
		/* Prune the item if either it is a "root pixmap" item and
		 * we want to prune them, or its surface is NULL because
		 * whichever object it created has been destroyed. */
		if ((root && (item->source_type == VTE_BG_SOURCE_ROOT)) ||
		    item->surface == NULL) {
			vte_bg_cache_item_free (item);
			bg->pvt->cache = g_list_delete_link(bg->pvt->cache, i);
		}
	}
}

static void
vte_bg_cache_prune(VteBg *bg)
{
	vte_bg_cache_prune_int(bg, FALSE);
}

static void item_surface_destroy_func(void *data)
{
	struct VteBgCacheItem *item = data;

	item->surface = NULL;
}

/* Add an item to the cache, instructing all of the objects therein to clear
   the field which holds a pointer to the object upon its destruction. */
static void
vte_bg_cache_add(VteBg *bg, struct VteBgCacheItem *item)
{
	vte_bg_cache_prune(bg);
	bg->pvt->cache = g_list_prepend(bg->pvt->cache, item);
	if (item->source_pixbuf != NULL) {
		g_object_add_weak_pointer(G_OBJECT(item->source_pixbuf),
					  (gpointer*)(void*)&item->source_pixbuf);
	}

	cairo_surface_set_user_data (item->surface, &item_surface_key, item,
				     item_surface_destroy_func);
}

/* Search for a match in the cache, and if found, return an object with an
   additional ref. */
static cairo_surface_t *
vte_bg_cache_search(VteBg *bg,
		    enum VteBgSourceType source_type,
		    const GdkPixbuf *source_pixbuf,
		    const char *source_file,
		    const PangoColor *tint,
		    double saturation)
{
	GList *i;

	vte_bg_cache_prune(bg);
	for (i = bg->pvt->cache; i != NULL; i = g_list_next(i)) {
		struct VteBgCacheItem *item = i->data;
		if (vte_bg_colors_equal(&item->tint_color, tint) &&
		    (saturation == item->saturation) &&
		    (source_type == item->source_type)) {
			switch (source_type) {
			case VTE_BG_SOURCE_ROOT:
				break;
			case VTE_BG_SOURCE_PIXBUF:
				if (item->source_pixbuf != source_pixbuf) {
					continue;
				}
				break;
			case VTE_BG_SOURCE_FILE:
				if (strcmp(item->source_file, source_file)) {
					continue;
				}
				break;
			default:
				g_assert_not_reached();
				break;
			}

			return cairo_surface_reference(item->surface);
		}
	}
	return NULL;
}

cairo_surface_t *
vte_bg_get_surface(VteBg *bg,
		   enum VteBgSourceType source_type,
		   GdkPixbuf *source_pixbuf,
		   const char *source_file,
		   const PangoColor *tint,
		   double saturation,
		   cairo_surface_t *other)
{
	struct VteBgCacheItem *item;
	GdkPixbuf *pixbuf;
	cairo_surface_t *cached, *source;
	cairo_t *cr;
	int width, height;

	if (source_type == VTE_BG_SOURCE_NONE) {
		return NULL;
	}

	cached = vte_bg_cache_search(bg, source_type,
				     source_pixbuf, source_file,
				     tint, saturation);
	if (cached != NULL) {
		return cached;
	}

	item = g_slice_new(struct VteBgCacheItem);
	item->source_type = source_type;
	item->source_pixbuf = NULL;
	item->source_file = NULL;
	item->tint_color = *tint;
	item->saturation = saturation;
	source = NULL;
	pixbuf = NULL;

	switch (source_type) {
	case VTE_BG_SOURCE_ROOT:
		break;
	case VTE_BG_SOURCE_PIXBUF:
		item->source_pixbuf = g_object_ref (source_pixbuf);
		pixbuf = g_object_ref (source_pixbuf);
		break;
	case VTE_BG_SOURCE_FILE:
		if ((source_file != NULL) && (strlen(source_file) > 0)) {
			item->source_file = g_strdup(source_file);
			pixbuf = gdk_pixbuf_new_from_file(source_file, NULL);
		}
		break;
	default:
		g_assert_not_reached();
		break;
	}

	if (pixbuf) {
		width = gdk_pixbuf_get_width(pixbuf);
		height = gdk_pixbuf_get_height(pixbuf);
	} else {
		width = cairo_xlib_surface_get_width(bg->root_surface);
		height = cairo_xlib_surface_get_height(bg->root_surface);
	}

	item->surface =
		cairo_surface_create_similar(other, CAIRO_CONTENT_COLOR_ALPHA,
					     width, height);

	cr = cairo_create (item->surface);
	cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
	if (pixbuf)
		gdk_cairo_set_source_pixbuf (cr, pixbuf, 0, 0);
	else
		cairo_set_source_surface (cr, bg->root_surface, 0, 0);
	cairo_paint (cr);

	if (saturation != 1.0) {
		cairo_set_source_rgba (cr, 
				       tint->red / 65535.,
				       tint->green / 65535.,
				       tint->blue / 65535.,
				       saturation);
		cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
		cairo_paint (cr);
	}
	cairo_destroy (cr);

	vte_bg_cache_add(bg, item);

	if (pixbuf)
		g_object_unref (pixbuf);

	return item->surface;
}
