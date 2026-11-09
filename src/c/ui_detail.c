#include "ui_detail.h"
#include "model.h"

static Window *s_window = NULL;
static Layer *s_header_layer = NULL;
static ScrollLayer *s_scroll_layer = NULL;
static TextLayer *s_text_layer = NULL;

static int s_current_index = 0;
static char s_header_buffer[64];
static char s_text_buffer[NOTE_TEXT_MAX_LEN + 1];

static void update_note_display(int index, bool scroll_to_top);

static void header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Background header bar
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack));
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Text inside header
  graphics_context_set_text_color(ctx, GColorWhite);
  GRect text_bounds = GRect(4, 2, bounds.size.w - 8, bounds.size.h - 4);
  graphics_draw_text(ctx, s_header_buffer,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     text_bounds,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter,
                     NULL);

  // Bottom dividing line
  graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorBlack));
  graphics_draw_line(ctx, GPoint(0, bounds.size.h - 1), GPoint(bounds.size.w, bounds.size.h - 1));
}

static void update_note_display(int index, bool scroll_to_top) {
  const VoiceNote *note = model_get_note(index);
  if (!note) {
    return;
  }

  s_current_index = index;

  // Format header: e.g. "1:45 PM • 1/5" or "Nov 07 • 1:45 PM"
  time_t ts = note->timestamp;
  struct tm *tm = localtime(&ts);
  char time_str[16];
  if (tm) {
    clock_copy_time_string(time_str, sizeof(time_str));
    // Check if today or older
    time_t now = time(NULL);
    struct tm *now_tm = localtime(&now);
    if (now_tm && tm->tm_year == now_tm->tm_year && tm->tm_yday == now_tm->tm_yday) {
      snprintf(s_header_buffer, sizeof(s_header_buffer), "%s  (%d/%d)",
               time_str, index + 1, model_get_note_count());
    } else {
      char date_str[16];
      strftime(date_str, sizeof(date_str), "%b %d", tm);
      snprintf(s_header_buffer, sizeof(s_header_buffer), "%s, %s  (%d/%d)",
               date_str, time_str, index + 1, model_get_note_count());
    }
  } else {
    snprintf(s_header_buffer, sizeof(s_header_buffer), "Note %d/%d",
             index + 1, model_get_note_count());
  }

  layer_mark_dirty(s_header_layer);

  // Update body text
  strncpy(s_text_buffer, note->text, sizeof(s_text_buffer) - 1);
  s_text_buffer[sizeof(s_text_buffer) - 1] = '\0';
  text_layer_set_text(s_text_layer, s_text_buffer);

  // Recalculate content height
  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  GSize max_size = GSize(scroll_bounds.size.w - 8, 2000);
  GSize text_size = graphics_text_layout_get_content_size(
    s_text_buffer,
    fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    GRect(0, 0, max_size.w, max_size.h),
    GTextOverflowModeWordWrap,
    GTextAlignmentLeft
  );

  int content_h = text_size.h + 20;
  if (content_h < scroll_bounds.size.h) {
    content_h = scroll_bounds.size.h;
  }

  text_layer_set_size(s_text_layer, GSize(scroll_bounds.size.w - 8, text_size.h + 10));
  scroll_layer_set_content_size(s_scroll_layer, GSize(scroll_bounds.size.w, content_h));

  if (scroll_to_top) {
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, 0), false);
  } else {
    // Scroll to bottom if coming from next note
    int max_scroll_y = -(content_h - scroll_bounds.size.h);
    if (max_scroll_y > 0) max_scroll_y = 0;
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, max_scroll_y), false);
  }
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  GSize content_size = scroll_layer_get_content_size(s_scroll_layer);
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);

  // Check if we are at or near the bottom of this note
  int current_bottom = scroll_bounds.size.h - offset.y;
  bool at_bottom = (content_size.h <= scroll_bounds.size.h) || (current_bottom >= content_size.h - 8);

  if (at_bottom) {
    // Go to next note (older note)
    if (s_current_index + 1 < model_get_note_count()) {
      vibes_short_pulse();
      update_note_display(s_current_index + 1, true);
    } else {
      vibes_double_pulse(); // End of list
    }
  } else {
    // Scroll down by 50px
    int new_y = offset.y - 50;
    int max_scroll_y = -(content_size.h - scroll_bounds.size.h);
    if (new_y < max_scroll_y) {
      new_y = max_scroll_y;
    }
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, new_y), true);
  }
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  GPoint offset = scroll_layer_get_content_offset(s_scroll_layer);

  // Check if we are at the top of this note
  bool at_top = (offset.y >= 0);

  if (at_top) {
    // Go to previous note (newer note)
    if (s_current_index > 0) {
      vibes_short_pulse();
      update_note_display(s_current_index - 1, false);
    } else {
      vibes_double_pulse(); // Beginning of list
    }
  } else {
    // Scroll up by 50px
    int new_y = offset.y + 50;
    if (new_y > 0) {
      new_y = 0;
    }
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, new_y), true);
  }
}

static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  window_stack_pop(true);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

static void window_load(Window *window) {
  Layer *root_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root_layer);

#if defined(PBL_PLATFORM_EMERY)
  int header_h = 24;
#elif defined(PBL_PLATFORM_CHALK)
  int header_h = 26;
#else
  int header_h = 20;
#endif

  // Header Layer
  s_header_layer = layer_create(GRect(0, 0, bounds.size.w, header_h));
  layer_set_update_proc(s_header_layer, header_update_proc);
  layer_add_child(root_layer, s_header_layer);

  // Scroll Layer below header
  GRect scroll_frame = GRect(0, header_h, bounds.size.w, bounds.size.h - header_h);
  s_scroll_layer = scroll_layer_create(scroll_frame);
  scroll_layer_set_callbacks(s_scroll_layer, (ScrollLayerCallbacks){
    .click_config_provider = click_config_provider
  });
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);
  layer_add_child(root_layer, scroll_layer_get_layer(s_scroll_layer));

  // Text Layer inside scroll layer
  s_text_layer = text_layer_create(GRect(4, 6, bounds.size.w - 8, scroll_frame.size.h));
  text_layer_set_font(s_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_color(s_text_layer, GColorBlack);
  text_layer_set_background_color(s_text_layer, GColorClear);
  text_layer_set_overflow_mode(s_text_layer, GTextOverflowModeWordWrap);
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_text_layer));

  window_set_background_color(window, GColorWhite);
}

static void window_unload(Window *window) {
  if (s_text_layer) {
    text_layer_destroy(s_text_layer);
    s_text_layer = NULL;
  }
  if (s_scroll_layer) {
    scroll_layer_destroy(s_scroll_layer);
    s_scroll_layer = NULL;
  }
  if (s_header_layer) {
    layer_destroy(s_header_layer);
    s_header_layer = NULL;
  }
}

void ui_detail_init(void) {
  if (!s_window) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
      .load = window_load,
      .unload = window_unload
    });
  }
}

void ui_detail_deinit(void) {
  if (s_window) {
    window_destroy(s_window);
    s_window = NULL;
  }
}

void ui_detail_show(int note_index) {
  if (!s_window) {
    ui_detail_init();
  }
  if (!window_is_loaded(s_window)) {
    window_stack_push(s_window, true);
  }
  update_note_display(note_index, true);
  if (!window_stack_contains_window(s_window)) {
    window_stack_push(s_window, true);
  }
}
