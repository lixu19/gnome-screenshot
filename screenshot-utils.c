/* screenshot-utils.c - common functions for GNOME Screenshot
 *
 * Copyright (C) 2001-2006  Jonathan Blandford <jrb@alum.mit.edu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 */

#include "config.h"
#include "screenshot-utils.h"

#include <X11/Xatom.h>
#include <gdk/gdkx.h>
#include <glib.h>
#include <glib/gi18n.h>

#ifdef HAVE_X11_EXTENSIONS_SHAPE_H
#include <X11/extensions/shape.h>
#endif

#include <X11/extensions/Xfixes.h>

static GtkWidget *selection_window;

#define SELECTION_NAME "_GNOME_PANEL_SCREENSHOT"


/* Functions to deal with properties from libwnck */
static char    * text_property_to_utf8 (const XTextProperty *prop);
static Window    get_window_property   (Window               xwindow,
					Atom                 atom);
static char     *get_text_property     (Window               xwindow,
					Atom                 atom);
static char     *get_utf8_property     (Window               xwindow,
					Atom                 atom);
static gboolean  get_atom_property     (Window               xwindow,
					Atom                 atom,
					Atom                *val);

static char *
text_property_to_utf8 (const XTextProperty *prop)
{
  char **list;
  int count;
  char *retval;
  
  list = NULL;

  count = gdk_text_property_to_utf8_list (gdk_x11_xatom_to_atom (prop->encoding),
                                          prop->format,
                                          prop->value,
                                          prop->nitems,
                                          &list);

  if (count == 0)
    return NULL;

  retval = list[0];
  list[0] = g_strdup (""); /* something to free */
  
  g_strfreev (list);

  return retval;
}

/* Borrowed from libwnck */
static Window
get_window_property (Window  xwindow,
		     Atom    atom)
{
  Atom type;
  int format;
  gulong nitems;
  gulong bytes_after;
  Window *w;
  int err, result;
  Window retval;

  gdk_error_trap_push ();

  type = None;
  result = XGetWindowProperty (gdk_display,
			       xwindow,
			       atom,
			       0, G_MAXLONG,
			       False, XA_WINDOW, &type, &format, &nitems,
			       &bytes_after, (unsigned char **) &w);  
  err = gdk_error_trap_pop ();

  if (err != Success ||
      result != Success)
    return None;
  
  if (type != XA_WINDOW)
    {
      XFree (w);
      return None;
    }

  retval = *w;
  XFree (w);
  
  return retval;
}

static char*
get_text_property (Window  xwindow,
		   Atom    atom)
{
  XTextProperty text;
  char *retval;
  
  gdk_error_trap_push ();

  text.nitems = 0;
  if (XGetTextProperty (gdk_display,
                        xwindow,
                        &text,
                        atom))
    {
      retval = text_property_to_utf8 (&text);

      if (text.nitems > 0)
        XFree (text.value);
    }
  else
    {
      retval = NULL;
    }
  
  gdk_error_trap_pop ();

  return retval;
}

static char *
get_utf8_property (Window  xwindow,
		   Atom    atom)
{
  Atom type;
  int format;
  gulong nitems;
  gulong bytes_after;
  guchar *val;
  int err, result;
  char *retval;
  Atom utf8_string;

  utf8_string = gdk_x11_get_xatom_by_name ("UTF8_STRING");

  gdk_error_trap_push ();
  type = None;
  val = NULL;
  result = XGetWindowProperty (gdk_display,
			       xwindow,
			       atom,
			       0, G_MAXLONG,
			       False, utf8_string,
			       &type, &format, &nitems,
			       &bytes_after, (guchar **)&val);  
  err = gdk_error_trap_pop ();

  if (err != Success ||
      result != Success)
    return NULL;
  
  if (type != utf8_string ||
      format != 8 ||
      nitems == 0)
    {
      if (val)
        XFree (val);
      return NULL;
    }

  if (!g_utf8_validate ((gchar *)val, nitems, NULL))
    {
      g_warning ("Property %s contained invalid UTF-8\n",
		 gdk_x11_get_xatom_name (atom));
      XFree (val);
      return NULL;
    }
  
  retval = g_strndup ((gchar *)val, nitems);
  
  XFree (val);
  
  return retval;
}

static gboolean
get_atom_property (Window  xwindow,
		   Atom    atom,
		   Atom   *val)
{
  Atom type;
  int format;
  gulong nitems;
  gulong bytes_after;
  unsigned char *a;
  int err, result;

  *val = 0;
  
  gdk_error_trap_push ();
  type = None;
  result = XGetWindowProperty (gdk_display,
			       xwindow,
			       atom,
			       0, G_MAXLONG,
			       False, XA_ATOM, &type, &format, &nitems,
			       &bytes_after, &a);  
  err = gdk_error_trap_pop ();
  if (err != Success ||
      result != Success)
    return FALSE;
  
  if (type != XA_ATOM)
    {
      XFree (a);
      return FALSE;
    }

  *val = *a;
  
  XFree (a);

  return TRUE;
}

/* To make sure there is only one screenshot taken at a time,
 * (Imagine key repeat for the print screen key) we hold a selection
 * until we are done taking the screenshot
 */
gboolean
screenshot_grab_lock (void)
{
  Atom selection_atom;
  GdkCursor *cursor;
  gboolean result = FALSE;

  selection_atom = gdk_x11_get_xatom_by_name (SELECTION_NAME);
  XGrabServer (GDK_DISPLAY ());
  if (XGetSelectionOwner (GDK_DISPLAY(), selection_atom) != None)
    goto out;

  selection_window = gtk_invisible_new ();
  gtk_widget_show (selection_window);

  if (!gtk_selection_owner_set (selection_window,
				gdk_atom_intern (SELECTION_NAME, FALSE),
				GDK_CURRENT_TIME))
    {
      gtk_widget_destroy (selection_window);
      selection_window = NULL;
      goto out;
    }

  result = TRUE;

 out:
  XUngrabServer (GDK_DISPLAY ());
  gdk_flush ();

  return result;
}

void
screenshot_release_lock (void)
{
  if (selection_window)
    {
      gtk_widget_destroy (selection_window);
      selection_window = NULL;
    }
  gdk_flush ();
}


static Window
find_toplevel_window (Window xid)
{
  Window root, parent, *children;
  unsigned int nchildren;

  do
    {
      if (XQueryTree (GDK_DISPLAY (), xid, &root,
		      &parent, &children, &nchildren) == 0)
	{
	  g_warning ("Couldn't find window manager window");
	  return None;
	}

      if (root == parent)
	return xid;

      xid = parent;
    }
  while (TRUE);
}

static Window
screenshot_find_active_window (void)
{
  Window retval = None;
  Window root_window;

  root_window = GDK_ROOT_WINDOW ();

  if (gdk_net_wm_supports (gdk_atom_intern ("_NET_ACTIVE_WINDOW", FALSE)))
    {
      retval = get_window_property (root_window,
				    gdk_x11_get_xatom_by_name ("_NET_ACTIVE_WINDOW"));
    }

  return retval;  
}
  
static gboolean
screenshot_window_is_desktop (Window xid)
{
  Window root_window = GDK_ROOT_WINDOW ();

  if (xid == root_window)
    return TRUE;

  if (gdk_net_wm_supports (gdk_atom_intern ("_NET_WM_WINDOW_TYPE", FALSE)))
    {
      gboolean retval;
      Atom property;

      retval = get_atom_property (xid,
				  gdk_x11_get_xatom_by_name ("_NET_WM_WINDOW_TYPE"),
				  &property);
      if (retval &&
	  property == gdk_x11_get_xatom_by_name ("_NET_WM_WINDOW_TYPE_DESKTOP"))
	return TRUE;
    }
  return FALSE;
}

/* We don't actually honor include_decoration here.  We need to search
 * for WM_STATE;
 */
static Window
screenshot_find_pointer_window (void)
{
  Window root_window, root_return, child;
  int unused;
  guint mask;

  root_window = GDK_ROOT_WINDOW ();

  XQueryPointer (GDK_DISPLAY (), root_window,
		 &root_return, &child, &unused,
		 &unused, &unused, &unused, &mask);

  return child;
}

#define MAXIMUM_WM_REPARENTING_DEPTH 4

/* adopted from eel code */
static Window
look_for_hint_helper (Window    xid,
		      Atom      property,
		      int       depth)
{
  Atom actual_type;
  int actual_format;
  gulong nitems, bytes_after;
  gulong *prop;
  Window root, parent, *children, window;
  unsigned int nchildren, i;

  if (XGetWindowProperty (GDK_DISPLAY (), xid, property, 0, 1,
			  False, AnyPropertyType, &actual_type,
			  &actual_format, &nitems, &bytes_after,
			  (gpointer) &prop) == Success
      && prop != NULL && actual_format == 32 && prop[0] == NormalState)
    {
      if (prop != NULL)
	{
	  XFree (prop);
	}

      return xid;
    }

  if (depth < MAXIMUM_WM_REPARENTING_DEPTH)
    {
      if (XQueryTree (GDK_DISPLAY (), xid, &root,
		      &parent, &children, &nchildren) != 0)
	{
	  window = None;

	  for (i = 0; i < nchildren; i++)
	    {
	      window = look_for_hint_helper (children[i],
					     property,
					     depth + 1);
	      if (window != None)
		break;
	    }

	  if (children != NULL)
	    XFree (children);

	  if (window)
	    return window;
	}
    }

  return None;
}

static Window
look_for_hint (Window xid,
	       Atom property)
{
  Window retval;

  retval = look_for_hint_helper (xid, property, 0);

  return retval;
}


Window
screenshot_find_current_window (gboolean include_decoration)
{
  Window current_window;

  /* First, search for _NET_ACTIVE_WINDOW */
  current_window = screenshot_find_active_window ();

  /* IF there's no _NET_ACTIVE_WINDOW, we fall back to returning the
   * Window that the cursor is in.*/
  if (current_window == None)
    current_window = screenshot_find_pointer_window ();

  if (current_window)
    {
      if (screenshot_window_is_desktop (current_window))
	/* if the current window is the desktop (eg. nautilus), we
	 * return None, as getting the whole screen makes more sense. */
	return None;

      /* Once we have a window, we walk the widget tree looking for
       * the appropriate window. */
      current_window = find_toplevel_window (current_window);
      if (! include_decoration)
	{
	  Window new_window;
	  new_window = look_for_hint (current_window, gdk_x11_get_xatom_by_name ("WM_STATE"));
	  if (new_window)
	    current_window = new_window;
	}
    }
  return current_window;
}

GdkPixbuf *
screenshot_get_pixbuf (Window w, gboolean include_pointer)
{
  GdkWindow *window, *root;
  GdkPixbuf *screenshot;
  gint x_real_orig, y_real_orig;
  gint x_orig, y_orig;
  gint real_width, real_height;
  gint width, height;

#ifdef HAVE_X11_EXTENSIONS_SHAPE_H
  XRectangle *rectangles;
  GdkPixbuf *tmp;
  int rectangle_count, rectangle_order, i;
#endif


  window = gdk_window_foreign_new (w);
  if (window == NULL)
    return NULL;
  
  root = gdk_window_foreign_new (GDK_ROOT_WINDOW ());
  gdk_drawable_get_size (window, &real_width, &real_height);
  gdk_window_get_origin (window, &x_real_orig, &y_real_orig);

  x_orig = x_real_orig;
  y_orig = y_real_orig;
  width = real_width;
  height = real_height;

  if (x_orig < 0)
    {
      width = width + x_orig;
      x_orig = 0;
    }
  if (y_orig < 0)
    {
      height = height + y_orig;
      y_orig = 0;
    }

  if (x_orig + width > gdk_screen_width ())
    width = gdk_screen_width () - x_orig;
  if (y_orig + height > gdk_screen_height ())
    height = gdk_screen_height () - y_orig;


#ifdef HAVE_X11_EXTENSIONS_SHAPE_H
  tmp = gdk_pixbuf_get_from_drawable (NULL, root, NULL,
				      x_orig, y_orig, 0, 0,
				      width, height);

  rectangles = XShapeGetRectangles (GDK_DISPLAY (), GDK_WINDOW_XWINDOW (window),
				    ShapeBounding, &rectangle_count, &rectangle_order);
  if (rectangle_count > 0)
    {
      gboolean has_alpha = gdk_pixbuf_get_has_alpha (tmp);

      screenshot = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8,
				   width, height);
      gdk_pixbuf_fill (screenshot, 0);
	
      for (i = 0; i < rectangle_count; i++)
	{
	  gint rec_x, rec_y;
	  gint rec_width, rec_height;
          gint y;

	  rec_x = rectangles[i].x;
	  rec_y = rectangles[i].y;
	  rec_width = rectangles[i].width;
	  rec_height = rectangles[i].height;

	  if (x_real_orig < 0)
	    {
	      rec_x += x_real_orig;
	      rec_x = MAX(rec_x, 0);
	      rec_width += x_real_orig;
	    }
	  if (y_real_orig < 0)
	    {
	      rec_y += y_real_orig;
	      rec_y = MAX(rec_y, 0);
	      rec_height += y_real_orig;
	    }

	  if (x_orig + rec_x + rec_width > gdk_screen_width ())
	    rec_width = gdk_screen_width () - x_orig - rec_x;
	  if (y_orig + rec_y + rec_height > gdk_screen_height ())
	    rec_height = gdk_screen_height () - y_orig - rec_y;

	  for (y = rec_y; y < rec_y + rec_height; y++)
	    {
              guchar *src_pixels, *dest_pixels;
              gint x;
	      
	      src_pixels = gdk_pixbuf_get_pixels (tmp) +
		y * gdk_pixbuf_get_rowstride(tmp) +
		rec_x * (has_alpha ? 4 : 3);
	      dest_pixels = gdk_pixbuf_get_pixels (screenshot) +
		y * gdk_pixbuf_get_rowstride (screenshot) +
		rec_x * 4;
				
	      for (x = 0; x < rec_width; x++)
		{
		  *dest_pixels++ = *src_pixels ++;
		  *dest_pixels++ = *src_pixels ++;
		  *dest_pixels++ = *src_pixels ++;
		  if (has_alpha)
		    *dest_pixels++ = *src_pixels++;
		  else
		    *dest_pixels++ = 255;
		}
	    }
	}
      g_object_unref (tmp);
    }
  else
    {
      screenshot = tmp;
    }
#else /* HAVE_X11_EXTENSIONS_SHAPE_H */
  screenshot = gdk_pixbuf_get_from_drawable (NULL, root, NULL,
					     x_orig, y_orig, 0, 0,
					     width, height);
#endif /* HAVE_X11_EXTENSIONS_SHAPE_H */

  if (include_pointer) 
    {
      XFixesCursorImage *cursor_image;

      cursor_image = XFixesGetCursorImage (GDK_DISPLAY ());
      
      if (cursor_image != NULL) 
        {
          GdkRectangle r1, r2;
          int cx, cy, i, j, k;
          int r, g, b, a;
          guchar *pixels, *p;
          int rowstride;
          unsigned long pixel;

          cx = cursor_image->x - cursor_image->xhot;
          cy = cursor_image->y - cursor_image->yhot;

          r1.x = x_real_orig;
          r1.y = y_real_orig;
          r1.width = real_width;
          r1.height = real_height;
          r2.x = cx;
          r2.y = cy;
          r2.width = cursor_image->width;
          r2.height = cursor_image->height;
          if (gdk_rectangle_intersect (&r1, &r2, &r2)) 
            {
              GdkPixbuf *cursor_pixbuf;

              cursor_pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB,
                                              TRUE, 8, 
                                              cursor_image->width,
                                               cursor_image->height);
              pixels = gdk_pixbuf_get_pixels (cursor_pixbuf);
              rowstride = gdk_pixbuf_get_rowstride (cursor_pixbuf);
              for (i = 0, k = 0; i < cursor_image->height; i++) 
                {
                  p = pixels + i * rowstride;
                  for (j = 0; j < cursor_image->width; j++, k++)
                    {
                      pixel = cursor_image->pixels[k];
                    
                      b = pixel & 0xff;
                      g = (pixel >> 8) & 0xff;
                      r = (pixel >> 16) & 0xff;
                      a = (pixel >> 24) & 0xff;

                      if (a != 0) 
                        {
                          p[0] = r * 255 / a;
                          p[1] = g * 255 / a;
                          p[2] = b * 255 / a;
                          p[3] = a;
                        }
                      else 
                        {
                          p[0] = p[1] = p[2] = p[3] = 0;
                        }

                      p += 4;
                    }
               }
 
              gdk_pixbuf_composite (cursor_pixbuf, screenshot,
                                    r2.x - x_real_orig, r2.y - y_real_orig, 
                                    r2.width, r2.height,
                                    cx - x_real_orig, cy - y_real_orig, 
                                    1.0, 1.0, 
                                    GDK_INTERP_BILINEAR,
                                    255);

              g_object_unref (cursor_pixbuf);
            }

          XFree (cursor_image);
        }
    }

  return screenshot;
}

gchar *
screenshot_get_window_title (Window w)
{
  gchar *name;

  w = find_toplevel_window (w);
  w = look_for_hint (w, gdk_x11_get_xatom_by_name ("WM_STATE"));

  if (w)
    {
      name = get_utf8_property (w, gdk_x11_get_xatom_by_name ("_NET_WM_NAME"));
      if (name)
	return name;

      name = get_text_property (w, gdk_x11_get_xatom_by_name ("WM_NAME"));

      if (name)
	return name;
  
      name = get_text_property (w, gdk_x11_get_xatom_by_name ("WM_CLASS"));

      if (name)
	return name;
    }

  return g_strdup (_("Untitled Window"));
}

void
screenshot_show_error_dialog (GtkWindow   *parent,
                              const gchar *message,
                              const gchar *detail)
{
  GtkWidget *dialog;
  
  g_return_if_fail ((parent == NULL) || (GTK_IS_WINDOW (parent)));
  g_return_if_fail (message != NULL);
  
  dialog = gtk_message_dialog_new (parent,
  				   GTK_DIALOG_DESTROY_WITH_PARENT,
  				   GTK_MESSAGE_ERROR,
  				   GTK_BUTTONS_OK,
  				   "%s", message);
  gtk_window_set_title (GTK_WINDOW (dialog), "");
  
  if (detail)
    gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
  					      "%s", detail);
  
  if (parent && parent->group)
    gtk_window_group_add_window (parent->group, GTK_WINDOW (dialog));
  
  gtk_dialog_run (GTK_DIALOG (dialog));
  
  gtk_widget_destroy (dialog);

}

void
screenshot_show_gerror_dialog (GtkWindow   *parent,
                               const gchar *message,
                               GError      *error)
{
  g_return_if_fail (parent == NULL || GTK_IS_WINDOW (parent));
  g_return_if_fail (message != NULL);
  g_return_if_fail (error != NULL);

  screenshot_show_error_dialog (parent, message, error->message);
  g_clear_error (&error);
}
