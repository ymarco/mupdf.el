#include <mupdf/fitz.h>
#include <mupdf/pdf.h> /* for pdf specifics and forms */
#include <mupdf/ucdn.h>

#include "from-webkit.h"
#include "mupdf-gtk.h"
#include <gtk/gtk.h>

static fz_context *ctx;

gboolean draw_callback(GtkWidget *widget, cairo_t *cr, Client *c) {

  cairo_surface_t *surface = c->image_surf;

  unsigned int width = cairo_image_surface_get_width(surface);
  unsigned int height = cairo_image_surface_get_height(surface);

  unsigned char *image = cairo_image_surface_get_data(surface);

  fz_irect whole_rect = {.x1 = width, .y1 = height};

  fz_pixmap *pixmap = fz_new_pixmap_with_bbox_and_data(
      ctx, c->doci->colorspace, whole_rect, NULL, 1, image);
  fz_clear_pixmap_with_value(ctx, pixmap, 0xFF);

  fz_device *draw_device = fz_new_draw_device(ctx, fz_identity, pixmap);
  fz_location *loc = &c->doci->location;
  Page *page = &c->doci->pages[loc->chapter][loc->page];
  fz_run_page(ctx, page->page, draw_device, page->draw_page_ctm, NULL);

  fz_close_device(ctx, draw_device);
  fz_drop_device(ctx, draw_device);
  fz_drop_pixmap(ctx, pixmap);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);

  if (c->has_mouse_event) {
    c->has_mouse_event = FALSE;
    // draw a circle where clicked
    GdkRGBA color;
    GtkStyleContext *style = gtk_widget_get_style_context(widget);
    gtk_render_background(style, cr, 0, 0, width, height);
    cairo_arc(cr, c->mouse_event_x, c->mouse_event_y, 20.0, 0, 2 * G_PI);
    gtk_style_context_get_color(style, gtk_style_context_get_state(style),
                                &color);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_fill(cr);
  }

  return FALSE;
}

static void allocate_pixmap(GtkWidget *widget, GdkRectangle *allocation,
                            Client *c) {
  cairo_surface_destroy(c->image_surf);
  c->image_surf = cairo_image_surface_create(
      CAIRO_FORMAT_RGB24, allocation->width, allocation->height);
}

static gboolean button_press_event(GtkWidget *widget, GdkEventButton *event,
                                   Client *c) {
  c->mouse_event_x = event->x;
  c->mouse_event_y = event->y;
  c->mouse_event_button = event->button;
  c->has_mouse_event = TRUE;
  gtk_widget_queue_draw(widget);
  return FALSE;
}

static void activate(GtkApplication *app, gpointer user_data) {
  GtkWidget *window;

  window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "Window");
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 900);
  Client *c = (Client *)user_data;

  c->container = gtk_drawing_area_new();

  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(c->container));

  g_signal_connect(G_OBJECT(c->container), "draw", G_CALLBACK(draw_callback),
                   c);
  g_signal_connect(G_OBJECT(c->container), "size-allocate",
                   G_CALLBACK(allocate_pixmap), c);
  // handle mouse hover and click
  gtk_widget_add_events(c->container,
                        GDK_EXPOSURE_MASK | GDK_LEAVE_NOTIFY_MASK |
                            GDK_BUTTON_PRESS_MASK | GDK_POINTER_MOTION_MASK |
                            GDK_POINTER_MOTION_HINT_MASK);
  /* gtk_signal_connect(GTK_OBJECT(c->container), "motion_notify_event", */
  /*                    (GtkSignalFunc)motion_notify_event, c); */
  g_signal_connect(G_OBJECT(c->container), "button_press_event",
                   G_CALLBACK(button_press_event), c);

  gtk_widget_show_all(window);
}

void load_doc(DocInfo *doci, char *filename, char *accel_filename) {
  // zero it all out - the short way of setting everything to NULL.
  memset(doci, 0, sizeof(*doci));
  strcpy(doci->filename, filename);
  if (accel_filename)
    strcpy(doci->accel, accel_filename);

  fz_try(ctx) doci->doc =
      fz_open_accelerated_document(ctx, doci->filename, doci->accel);
  fz_catch(ctx) {
    fprintf(stderr, "cannot open document: %s\n", fz_caught_message(ctx));
    fz_drop_context(ctx);
    exit(EXIT_FAILURE);
  }
  fz_location loc = {0, 0};
  doci->location = loc;
  doci->colorspace = fz_device_rgb(ctx);
  doci->zoom = 100.0f;
  /* Count the number of pages. */
  fz_try(ctx) {
    doci->chapter_count = fz_count_chapters(ctx, doci->doc);
    if (!(doci->pages = calloc(sizeof(Page *), doci->chapter_count))) {
      fz_throw(ctx, 1, "Can't allocate");
    }
    if (!(doci->page_count_for_chapter =
              calloc(sizeof(int *), doci->chapter_count))) {
      fz_throw(ctx, 1, "Can't allocate");
    }
  }
  fz_catch(ctx) {
    fprintf(stderr, "cannot count number of pages: %s\n",
            fz_caught_message(ctx));
    fz_drop_document(ctx, doci->doc);
    fz_drop_context(ctx);
    exit(EXIT_FAILURE);
  }
}

void drop_page(Page *page) {
  fz_drop_stext_page(ctx, page->page_text);
  fz_drop_separations(ctx, page->seps);
  fz_drop_link(ctx, page->links);
  fz_drop_page(ctx, page->page);
  fz_drop_display_list(ctx, page->display_list);
  memset(page, 0, sizeof(*page));
}

void load_page(DocInfo *doci, fz_location location) {

  doci->location = location;
  Page *chapter_pages = doci->pages[location.chapter];
  Page *page;

  if (!chapter_pages) {
    doci->page_count_for_chapter[location.chapter] =
        fz_count_chapter_pages(ctx, doci->doc, location.chapter);
    chapter_pages = doci->pages[location.chapter] = calloc(
        sizeof(Page), doci->page_count_for_chapter[doci->location.chapter]);
    page = &chapter_pages[location.page];
  } else if ((page = &chapter_pages[location.page])) {
    drop_page(page);
  }

  page->page =
      fz_load_chapter_page(ctx, doci->doc, location.chapter, location.page);
  page->page_text = fz_new_stext_page_from_page(ctx, page->page, NULL);
  page->seps = NULL; // TODO seps
  page->links = fz_load_links(ctx, page->page);
  page->page_bounds = fz_bound_page(ctx, page->page);
  page->display_list = fz_new_display_list(ctx, page->page_bounds);
  page->draw_page_ctm =
      fz_transform_page(page->page_bounds, doci->zoom, doci->rotate);
  page->draw_page_bounds =
      fz_transform_rect(page->page_bounds, page->draw_page_ctm);
}

int main(int argc, char **argv) {
  ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
  GtkApplication *app;
  int status;

  fz_try(ctx) { fz_register_document_handlers(ctx); }
  fz_catch(ctx) {
    fprintf(stderr, "cannot register document handlers: %s\n",
            fz_caught_message(ctx));
    fz_drop_context(ctx);
    return EXIT_FAILURE;
  }

  DocInfo _doci;
  DocInfo *doci = &_doci;
  // TODO accel logic
  load_doc(doci, "./cancel.pdf", NULL);
  fz_location loc = {0, 1};
  fz_try(ctx) { load_page(doci, loc); }
  fz_catch(ctx) {
    fprintf(stderr, "can't load page");
    exit(EXIT_FAILURE);
  }
  fprintf(stderr, "chapters: %d, chap %d pages: %d\n", doci->chapter_count,
          loc.chapter, doci->page_count_for_chapter[loc.chapter]);

  app = gtk_application_new("org.gtk.example", G_APPLICATION_FLAGS_NONE);
  Client client;
  Client *c = &client;
  c->doci = doci;
  g_signal_connect(app, "activate", G_CALLBACK(activate), c);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return status;
}
