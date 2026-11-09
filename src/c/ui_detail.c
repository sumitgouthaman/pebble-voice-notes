#include "ui_detail.h"
#include "model.h"

static Window *s_window = NULL;
static Layer *s_header_layer = NULL;
static ScrollLayer *s_scroll_layer = NULL;
static TextLayer *s_time_layer = NULL;
static TextLayer *s_text_layer = NULL;

static int s_current_index = 0;
static char s_header_buffer[32];
static char s_time_buffer[80];
static char s_text_buffer[NOTE_TEXT_MAX_LEN + 1];

static void update_note_display(int index, bool scroll_to_top);

static void format_note_clock(const struct tm *note_tm, char *buf, size_t buf_size) {
  if (clock_is_24h_style()) {
    strftime(buf, buf_size, "%H:%M", note_tm);
  } else {
    strftime(buf, buf_size, "%I:%M %p", note_tm);
    if (buf[0] == '0') {
      memmove(buf, &buf[1], strlen(buf));
    }
  }
}

static void format_relative_time(time_t note_ts, time_t now, char *buf, size_t buf_size) {
  int32_t diff = (int32_t)(now - note_ts);
  if (diff < 0) {
    diff = 0;
  }

  if (diff < 60) {
    snprintf(buf, buf_size, "(just now)");
  } else if (diff < 3600) {
    int mins = (int)(diff / 60);
    snprintf(buf, buf_size, "(%d %s ago)", mins, mins == 1 ? "min" : "mins");
  } else if (diff < 86400) {
    int hrs = (int)(diff / 3600);
    snprintf(buf, buf_size, "(%d %s ago)", hrs, hrs == 1 ? "hr" : "hrs");
  } else {
    int days = (int)(diff / 86400);
    if (days < 14) {
      snprintf(buf, buf_size, "(%d %s ago)", days, days == 1 ? "day" : "days");
    } else {
      int weeks = days / 7;
      snprintf(buf, buf_size, "(%d %s ago)", weeks, weeks == 1 ? "wk" : "wks");
    }
  }
}

static void header_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  // Background header bar
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack));
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Text inside header
  graphics_context_set_text_color(ctx, GColorWhite);
  GRect text_bounds = GRect(4, 1, bounds.size.w - 8, bounds.size.h - 2);
  graphics_draw_text(ctx, s_header_buffer,
                     fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                     text_bounds,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentCenter,
                     NULL);
}

static void update_note_display(int index, bool scroll_to_top) {
  const VoiceNote *note = model_get_note(index);
  if (!note) {
    return;
  }

  s_current_index = index;
  snprintf(s_header_buffer, sizeof(s_header_buffer), "Note %d of %d",
           index + 1, model_get_note_count());
  layer_mark_dirty(s_header_layer);

  // Copy struct tm by value before calling localtime() a second time
  // (localtime returns a shared static struct in newlib)
  time_t ts = note->timestamp;
  time_t now = time(NULL);

  struct tm note_tm;
  bool has_note_tm = false;
  struct tm *tm_ptr = localtime(&ts);
  if (tm_ptr) {
    note_tm = *tm_ptr;
    has_note_tm = true;
  }

  struct tm now_tm;
  bool has_now_tm = false;
  struct tm *now_ptr = localtime(&now);
  if (now_ptr) {
    now_tm = *now_ptr;
    has_now_tm = true;
  }

  char rel_str[24];
  format_relative_time(ts, now, rel_str, sizeof(rel_str));

  if (has_note_tm) {
    char clock_str[16];
    format_note_clock(&note_tm, clock_str, sizeof(clock_str));

    if (has_now_tm && note_tm.tm_year == now_tm.tm_year && note_tm.tm_yday == now_tm.tm_yday) {
      snprintf(s_time_buffer, sizeof(s_time_buffer), "Today, %s\n%s", clock_str, rel_str);
    } else if (has_now_tm &&
               ((note_tm.tm_year == now_tm.tm_year && note_tm.tm_yday == now_tm.tm_yday - 1) ||
                (note_tm.tm_year == now_tm.tm_year - 1 && now_tm.tm_yday == 0 && note_tm.tm_yday >= 364))) {
      snprintf(s_time_buffer, sizeof(s_time_buffer), "Yesterday, %s\n%s", clock_str, rel_str);
    } else {
      char date_str[24];
      if (has_now_tm && note_tm.tm_year == now_tm.tm_year) {
        strftime(date_str, sizeof(date_str), "%a, %b %d", &note_tm);
      } else {
        strftime(date_str, sizeof(date_str), "%b %d, %Y", &note_tm);
      }
      snprintf(s_time_buffer, sizeof(s_time_buffer), "%s, %s\n%s", date_str, clock_str, rel_str);
    }
  } else {
    snprintf(s_time_buffer, sizeof(s_time_buffer), "%s", rel_str);
  }

  text_layer_set_text(s_time_layer, s_time_buffer);

  // Update body text
  strncpy(s_text_buffer, note->text, sizeof(s_text_buffer) - 1);
  s_text_buffer[sizeof(s_text_buffer) - 1] = '\0';
  text_layer_set_text(s_text_layer, s_text_buffer);

  // Measure and lay out timestamp block followed by note text
  GRect scroll_bounds = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer));
  int text_w = scroll_bounds.size.w - 12;

  GSize time_size = graphics_text_layout_get_content_size(
    s_time_buffer,
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(0, 0, text_w, 200),
    GTextOverflowModeWordWrap,
    GTextAlignmentLeft
  );

  GSize body_size = graphics_text_layout_get_content_size(
    s_text_buffer,
    fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
    GRect(0, 0, text_w, 2000),
    GTextOverflowModeWordWrap,
    GTextAlignmentLeft
  );

  int time_h = time_size.h + 6;
  int body_y = 4 + time_h + 4;
  int body_h = body_size.h + 12;

  layer_set_frame(text_layer_get_layer(s_time_layer), GRect(6, 4, text_w, time_h));
  layer_set_frame(text_layer_get_layer(s_text_layer), GRect(6, body_y, text_w, body_h));

  int content_h = body_y + body_h + 8;
  if (content_h < scroll_bounds.size.h) {
    content_h = scroll_bounds.size.h;
  }
  scroll_layer_set_content_size(s_scroll_layer, GSize(scroll_bounds.size.w, content_h));

  if (scroll_to_top) {
    scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, 0), false);
  } else {
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
  int header_h = 22;
#elif defined(PBL_PLATFORM_CHALK)
  int header_h = 24;
#else
  int header_h = 18;
#endif

  // Top Header Layer ("Note X of Y")
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

  // Prominent Date/Time + Relative Time Layer at top of scroll view
  s_time_layer = text_layer_create(GRect(6, 4, bounds.size.w - 12, 44));
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_color(s_time_layer, PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_overflow_mode(s_time_layer, GTextOverflowModeWordWrap);
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_time_layer));

  // Note Body Text Layer below timestamp
  s_text_layer = text_layer_create(GRect(6, 52, bounds.size.w - 12, scroll_frame.size.h));
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
  if (s_time_layer) {
    text_layer_destroy(s_time_layer);
    s_time_layer = NULL;
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
