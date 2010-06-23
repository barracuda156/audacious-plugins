/*
 *  Copyright (C) 2010 William Pitcock <nenolod@atheme.org>.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses>.
 *
 *  The Audacious team does not consider modular code linking to
 *  Audacious or using our public API to be a derived work.
 */

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include <audacious/plugin.h>
#include <libaudgui/libaudgui.h>
#include <libaudgui/libaudgui-gtk.h>

#include <math.h>

#include "gtkui_cfg.h"
#include "ui_gtk.h"
#include "ui_playlist_widget.h"
#include "ui_playlist_model.h"
#include "ui_manager.h"
#include "ui_infoarea.h"

#define DEFAULT_ARTWORK DATA_DIR "/images/audio.png"
#define STREAM_ARTWORK DATA_DIR "/images/streambrowser-64x64.png"
#define ICON_SIZE 64
#define SPECT_BANDS 12
#define VIS_OFFSET (12 * SPECT_BANDS + 12)

typedef struct {
    GtkWidget *parent;

    gchar * title, * artist, * album;

    struct {
        gfloat title;
        gfloat artist;
        gfloat album;
        gfloat artwork;
    } alpha;

    gint fadein_timeout;
    gint fadeout_timeout;
    guint8 visdata[20];

    InputPlayback *playback;
    GdkPixbuf *pb;
} UIInfoArea;

static const gfloat alpha_step = 0.10;

static void ui_infoarea_draw_visualizer (UIInfoArea * area);

/****************************************************************************/

static void
ui_infoarea_visualization_timeout(gpointer hook_data, UIInfoArea *area)
{
    VisNode *vis = (VisNode*) hook_data;
    gint16 mono_freq[2][256];

    const int xscale[] = { 0, 2, 3, 5, 6, 11,
                           20, 41, 62, 82, 143, 255 };

    aud_calc_mono_freq(mono_freq, vis->data, vis->nch);

    for (auto gint i = 0; i < 12; i++) {
        gint y = mono_freq[0][xscale[i]] / 128;
        area->visdata[i] = MAX (area->visdata[i] - 3, CLAMP (y, 0, 64));
    }

#if GTK_CHECK_VERSION (2, 20, 0)
    if (gtk_widget_is_drawable (area->parent))
#else
    if (GTK_WIDGET_DRAWABLE (area->parent))
#endif
        ui_infoarea_draw_visualizer (area);
}

static void vis_clear_cb (void * hook_data, UIInfoArea * area)
{
    memset (area->visdata, 0, 20);
}

/****************************************************************************/

static void ui_infoarea_draw_text (UIInfoArea * area, gint x, gint y, gint
 width, gfloat alpha, const gchar * font, const gchar * text)
{
    cairo_t *cr;
    PangoLayout *pl;
    PangoFontDescription *desc;
    gchar *str;

    str = g_markup_escape_text(text, -1);

    cr = gdk_cairo_create(area->parent->window);
    cairo_move_to(cr, x, y);
    cairo_set_operator(cr, CAIRO_OPERATOR_ATOP);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, alpha);

    desc = pango_font_description_from_string(font);
    pl = gtk_widget_create_pango_layout(area->parent, NULL);
    pango_layout_set_markup(pl, str, -1);
    pango_layout_set_font_description(pl, desc);
    pango_font_description_free(desc);
    pango_layout_set_width (pl, width * PANGO_SCALE);
    pango_layout_set_ellipsize (pl, PANGO_ELLIPSIZE_END);

    AUDDBG("Drawing %s to %d, %d at %p layout %p\n", text, x, y, cr, pl);

    pango_cairo_show_layout(cr, pl);

    cairo_destroy(cr);
    g_object_unref(pl);
    g_free(str);
}

/****************************************************************************/

static struct {
    guint8 red;
    guint8 green;
    guint8 blue;
} colors[] = {
    { 0xff, 0xb1, 0x6f },
    { 0xff, 0xc8, 0x7f },
    { 0xff, 0xcf, 0x7e },
    { 0xf6, 0xe6, 0x99 },
    { 0xf1, 0xfc, 0xd4 },
    { 0xbd, 0xd8, 0xab },
    { 0xcd, 0xe6, 0xd0 },
    { 0xce, 0xe0, 0xea },
    { 0xd5, 0xdd, 0xea },
    { 0xee, 0xc1, 0xc8 },
    { 0xee, 0xaa, 0xb7 },
    { 0xec, 0xce, 0xb6 },
};

static void ui_infoarea_draw_visualizer (UIInfoArea * area)
{
    GtkAllocation alloc;
    cairo_t *cr;

    gtk_widget_get_allocation(GTK_WIDGET(area->parent), &alloc);
    cr = gdk_cairo_create(area->parent->window);

    for (auto gint i = 0; i < SPECT_BANDS; i++)
    {
        gint x, y, w, h;

        x = alloc.width - VIS_OFFSET + 12 * i;
        w = 10;

        y = 11;
        h = 64 - area->visdata[i];

        cairo_set_source_rgba (cr, 0, 0, 0, area->alpha.title);
        cairo_rectangle (cr, x, y, w, h);
        cairo_fill (cr);

        y = 11 + 64 - area->visdata[i];
        h = area->visdata[i];

        cairo_set_source_rgba (cr, colors[i].red / 255.0, colors[i].green /
         255.0, colors[i].blue / 255.0, area->alpha.title);
        cairo_rectangle (cr, x, y, w, h);
        cairo_fill (cr);
    }

    cairo_destroy(cr);
}

static GdkPixbuf * get_current_album_art (void)
{
    gint playlist = aud_playlist_get_playing ();
    gint position = aud_playlist_get_position (playlist);
    const gchar * filename = aud_playlist_entry_get_filename (playlist, position);
    InputPlugin * decoder = aud_playlist_entry_get_decoder (playlist, position);
    void * data;
    gint size;
    GdkPixbuf * pixbuf;

    if (filename == NULL || decoder == NULL || ! aud_file_read_image (filename,
     decoder, & data, & size))
        return NULL;

    pixbuf = audgui_pixbuf_from_data (data, size);
    g_free (data);
    return pixbuf;
}

void
ui_infoarea_draw_album_art(UIInfoArea *area)
{
    GError *err = NULL;
    cairo_t *cr;

    if (area->title == NULL)
        return;

    if (area->pb == NULL)
    {
        if (audacious_drct_get_playing ())
            area->pb = get_current_album_art ();

        if (area->pb == NULL)
            area->pb = gdk_pixbuf_new_from_file (DEFAULT_ARTWORK, & err);

        if (area->pb != NULL)
            audgui_pixbuf_scale_within (& area->pb, ICON_SIZE);
    }

    if (area->pb == NULL)
        return;

    cr = gdk_cairo_create(area->parent->window);
    gdk_cairo_set_source_pixbuf(cr, area->pb, 10.0, 10.0);
    cairo_paint_with_alpha(cr, area->alpha.artwork);

    cairo_destroy(cr);
}

void
ui_infoarea_draw_title(UIInfoArea *area)
{
    GtkAllocation alloc;
    gint width;

    gtk_widget_get_allocation (area->parent, & alloc);
    width = alloc.width - (86 + VIS_OFFSET + 6);

    if (area->title != NULL)
        ui_infoarea_draw_text (area, 86, 8, width, area->alpha.title, "Sans 20",
         area->title);

    if (area->artist != NULL)
        ui_infoarea_draw_text (area, 86, 42, width, area->alpha.artist,
         "Sans 9", area->artist);

    if (area->album != NULL)
        ui_infoarea_draw_text (area, 86, 58, width, area->alpha.album, "Sans 9",
         area->album);
}

void
ui_infoarea_draw_background(UIInfoArea *area)
{
    GtkWidget *evbox;
    GtkAllocation alloc;
    cairo_t *cr;

    g_return_if_fail(area != NULL);

    evbox = area->parent;
    cr = gdk_cairo_create(evbox->window);

    gtk_widget_get_allocation(GTK_WIDGET(evbox), &alloc);

    cairo_rectangle(cr, 0, 0, alloc.width, alloc.height);
    cairo_paint(cr);

    cairo_destroy(cr);
}

gboolean
ui_infoarea_expose_event(UIInfoArea *area, GdkEventExpose *event, gpointer unused)
{
    ui_infoarea_draw_background(area);
    ui_infoarea_draw_album_art(area);
    ui_infoarea_draw_title(area);
    ui_infoarea_draw_visualizer(area);

    return TRUE;
}

static gboolean
ui_infoarea_do_fade_in(UIInfoArea *area)
{
    gboolean ret = FALSE;

    if (area->alpha.title < 1.0)
    {
        area->alpha.title += alpha_step;
        ret = TRUE;
    }

    if (area->alpha.artist < 1.0)
    {
        area->alpha.artist += alpha_step;
        ret = TRUE;
    }

    if (area->alpha.album < 0.7)
    {
        area->alpha.album += alpha_step;
        ret = TRUE;
    }

    if (area->alpha.artwork < 0.7)
    {
        area->alpha.artwork += alpha_step;
        ret = TRUE;
    }

    gtk_widget_queue_draw(GTK_WIDGET(area->parent));

    if (ret == FALSE)
        area->fadein_timeout = 0;

    if (ret == FALSE)
        AUDDBG("fadein complete\n");

    return ret;
}

static gboolean
ui_infoarea_do_fade_out(UIInfoArea *area)
{
    gboolean ret = FALSE;

    if (area->alpha.title > 0.0)
    {
        area->alpha.title -= alpha_step;
        ret = TRUE;
    }

    if (area->alpha.artist > 0.0)
    {
        area->alpha.artist -= alpha_step;
        ret = TRUE;
    }

    if (area->alpha.album > 0.0)
    {
        area->alpha.album -= alpha_step;
        ret = TRUE;
    }

    if (area->alpha.artwork > 0.0)
    {
        area->alpha.artwork -= alpha_step;
        ret = TRUE;
    }

    gtk_widget_queue_draw(GTK_WIDGET(area->parent));

    if (ret == FALSE) {
        area->fadeout_timeout = 0;

        AUDDBG("fadeout complete\n");
    }

    return ret;
}

void
ui_infoarea_set_title(gpointer unused, UIInfoArea *area)
{
    gint playlist = aud_playlist_get_playing ();
    gint entry = aud_playlist_get_position (playlist);
    const Tuple * tuple = aud_playlist_entry_get_tuple (playlist, entry);
    const gchar * s;

    g_free (area->title);
    area->title = NULL;
    g_free (area->artist);
    area->artist = NULL;
    g_free (area->album);
    area->album = NULL;

    if (tuple && (s = tuple_get_string (tuple, FIELD_TITLE, NULL)))
        area->title = g_strdup (s);
    else
        area->title = g_strdup (aud_playlist_entry_get_title (playlist, entry));

    if (tuple && (s = tuple_get_string (tuple, FIELD_ARTIST, NULL)))
        area->artist = g_strdup (s);

    if (tuple && (s = tuple_get_string (tuple, FIELD_ALBUM, NULL)))
        area->album = g_strdup (s);

    area->alpha.artist = area->alpha.album = area->alpha.artwork = area->alpha.title = 0.0;

    if (!area->fadein_timeout)
        area->fadein_timeout = g_timeout_add(10, (GSourceFunc) ui_infoarea_do_fade_in, area);

    if (area->fadeout_timeout) {
        g_source_remove(area->fadeout_timeout);
        area->fadeout_timeout = 0;
    }

    gtk_widget_queue_draw(GTK_WIDGET(area->parent));
}


void
ui_infoarea_playback_start(InputPlayback *playback, UIInfoArea *area)
{
    g_return_if_fail(area != NULL);

    area->playback = playback;

    if (area->pb != NULL)
    {
        g_object_unref(area->pb);
        area->pb = NULL;
    }
}

void
ui_infoarea_playback_stop(gpointer unused, UIInfoArea *area)
{
    g_return_if_fail(area != NULL);

    if (!area->fadeout_timeout)
        area->fadeout_timeout = g_timeout_add(10, (GSourceFunc) ui_infoarea_do_fade_out, area);

    gtk_widget_queue_draw(GTK_WIDGET(area->parent));
}

static void destroy_cb (GtkObject * parent, UIInfoArea * area)
{
    aud_hook_dissociate ("title change", (HookFunction) ui_infoarea_set_title);
    aud_hook_dissociate ("playback begin", (HookFunction)
     ui_infoarea_playback_start);
    aud_hook_dissociate ("playback stop", (HookFunction)
     ui_infoarea_playback_stop);
    aud_hook_dissociate ("visualization timeout", (HookFunction)
     ui_infoarea_visualization_timeout);
    aud_hook_dissociate ("visualization clear", (HookFunction) vis_clear_cb);

    g_free (area->title);
    g_free (area->artist);
    g_free (area->album);

    if (area->pb != NULL)
        g_object_unref (area->pb);

    g_slice_free (UIInfoArea, area);
}

GtkWidget * ui_infoarea_new (void)
{
    UIInfoArea *area;
    GtkWidget *evbox;

    area = g_slice_new0(UIInfoArea);

    evbox = gtk_event_box_new();
    area->parent = evbox;

    gtk_widget_set_size_request(GTK_WIDGET(evbox), -1, 84);

    g_signal_connect_swapped(area->parent, "expose-event",
                             G_CALLBACK(ui_infoarea_expose_event), area);

    aud_hook_associate("title change", (HookFunction) ui_infoarea_set_title, area);
    aud_hook_associate("playback begin", (HookFunction) ui_infoarea_playback_start, area);
    aud_hook_associate("playback stop", (HookFunction) ui_infoarea_playback_stop, area);
    aud_hook_associate("visualization timeout", (HookFunction) ui_infoarea_visualization_timeout, area);
    aud_hook_associate("visualization clear", (HookFunction) vis_clear_cb, area);

    g_signal_connect (area->parent, "destroy", (GCallback) destroy_cb, area);

    return area->parent;
}
