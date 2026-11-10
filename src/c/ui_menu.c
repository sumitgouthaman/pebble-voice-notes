#include "ui_menu.h"
#include "model.h"
#include "dictation.h"
#include "ui_detail.h"

static Window *s_window = NULL;
static MenuLayer *s_menu_layer = NULL;

#if defined(PBL_PLATFORM_EMERY)
  #define HEADER_HEIGHT 26
  #define ACTION_ROW_HEIGHT 56
  #define NOTE_ROW_HEIGHT 70
#elif defined(PBL_PLATFORM_CHALK)
  #define HEADER_HEIGHT 26
  #define ACTION_ROW_HEIGHT 52
  #define NOTE_ROW_HEIGHT 64
#else
  #define HEADER_HEIGHT 24
  #define ACTION_ROW_HEIGHT 50
  #define NOTE_ROW_HEIGHT 62
#endif

static uint16_t menu_get_num_sections_callback(MenuLayer *menu_layer, void *data) {
  // Section 0 is always the Action section (+ New Note)
  // Subsequent sections are day sections
  return 1 + model_get_day_section_count();
}

static uint16_t menu_get_num_rows_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  if (section_index == 0) {
    return 1; // Row: "+ Take Voice Note"
  }

  const DaySection *sec = model_get_day_section(section_index - 1);
  return sec ? sec->count : 0;
}

static int16_t menu_get_header_height_callback(MenuLayer *menu_layer, uint16_t section_index, void *data) {
  if (section_index == 0) {
    return 0; // No header for action row
  }
  return HEADER_HEIGHT;
}

static int16_t menu_get_cell_height_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (cell_index->section == 0) {
    return ACTION_ROW_HEIGHT;
  }
  return NOTE_ROW_HEIGHT;
}

static void menu_draw_header_callback(GContext *ctx, const Layer *cell_layer, uint16_t section_index, void *data) {
  if (section_index == 0) {
    return;
  }

  const DaySection *sec = model_get_day_section(section_index - 1);
  if (!sec) {
    return;
  }

  GRect bounds = layer_get_bounds(cell_layer);

  // Section header background
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack));
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Section header text
  graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite));
  GRect text_frame = GRect(6, 0, bounds.size.w - 12, bounds.size.h - 2);
  graphics_draw_text(ctx, sec->title,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     text_frame,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft,
                     NULL);
}

static void menu_draw_row_callback(GContext *ctx, const Layer *cell_layer, MenuIndex *cell_index, void *data) {
  GRect bounds = layer_get_bounds(cell_layer);
  bool is_highlighted = menu_cell_layer_is_highlighted(cell_layer);

  if (cell_index->section == 0) {
    // Action cell: "+ Take Voice Note"
    GColor bg_color = is_highlighted ?
      PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack) : GColorWhite;
    GColor title_color = is_highlighted ?
      GColorWhite : PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
    GColor sub_color = is_highlighted ?
      GColorWhite : GColorBlack;

    graphics_context_set_fill_color(ctx, bg_color);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    graphics_context_set_text_color(ctx, title_color);
    GRect title_rect = GRect(8, 1, bounds.size.w - 16, 22);
    graphics_draw_text(ctx, "+ Take Voice Note",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       title_rect,
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft,
                       NULL);

    graphics_context_set_text_color(ctx, sub_color);
    GRect sub_rect = GRect(8, 22, bounds.size.w - 16, 18);
    graphics_draw_text(ctx, "Tap to record memo",
                       fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       sub_rect,
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentLeft,
                       NULL);
    return;
  }

  // Note cell
  const VoiceNote *note = model_get_note_by_section_and_row(cell_index->section - 1, cell_index->row);
  if (!note) {
    return;
  }

  GColor bg_color = is_highlighted ?
    PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack) : GColorWhite;
  GColor time_color = is_highlighted ?
    GColorWhite : PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
  // High-contrast black/white for note body (no gray dithering)
  GColor text_color = is_highlighted ? GColorWhite : GColorBlack;

  graphics_context_set_fill_color(ctx, bg_color);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Format time (e.g. "1:45 PM" or "13:45")
  char time_str[16];
  time_t ts = note->timestamp;
  struct tm *tm = localtime(&ts);
  if (tm) {
    if (clock_is_24h_style()) {
      strftime(time_str, sizeof(time_str), "%H:%M", tm);
    } else {
      strftime(time_str, sizeof(time_str), "%I:%M %p", tm);
      if (time_str[0] == '0') {
        memmove(time_str, &time_str[1], strlen(time_str));
      }
    }
  } else {
    snprintf(time_str, sizeof(time_str), "Note");
  }

  // Draw prominent time header
  graphics_context_set_text_color(ctx, time_color);
  GRect time_rect = GRect(8, 0, bounds.size.w - 16, 22);
  graphics_draw_text(ctx, time_str,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     time_rect,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft,
                     NULL);

  // Draw 2-line prominent note text in bold high-contrast font
  graphics_context_set_text_color(ctx, text_color);
  GRect text_rect = GRect(8, 20, bounds.size.w - 16, bounds.size.h - 22);
  graphics_draw_text(ctx, note->text,
                     fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     text_rect,
                     GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft,
                     NULL);

  // Subtle bottom border between note rows when unhighlighted
  if (!is_highlighted) {
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack));
    graphics_draw_line(ctx, GPoint(8, bounds.size.h - 1), GPoint(bounds.size.w - 8, bounds.size.h - 1));
  }
}

static void menu_select_callback(MenuLayer *menu_layer, MenuIndex *cell_index, void *data) {
  if (cell_index->section == 0) {
    // Record new note
    dictation_start();
  } else {
    // Open full-screen note viewer
    int global_idx = model_get_global_index(cell_index->section - 1, cell_index->row);
    if (global_idx >= 0) {
      ui_detail_show(global_idx);
    }
  }
}

void ui_menu_reload(void) {
  if (!s_menu_layer) {
    return;
  }

  // Safe cursor bounds clamping before reload (Gotcha 4.D)
  MenuIndex current = menu_layer_get_selected_index(s_menu_layer);
  int num_sections = menu_get_num_sections_callback(s_menu_layer, NULL);

  if (current.section >= num_sections) {
    menu_layer_set_selected_index(s_menu_layer, MenuIndex(0, 0), MenuRowAlignTop, false);
  } else {
    int rows_in_sec = menu_get_num_rows_callback(s_menu_layer, current.section, NULL);
    if (current.row >= rows_in_sec) {
      menu_layer_set_selected_index(s_menu_layer, MenuIndex(current.section, 0), MenuRowAlignTop, false);
    }
  }

  menu_layer_reload_data(s_menu_layer);
}

static void window_load(Window *window) {
  Layer *root_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root_layer);

  s_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_sections = menu_get_num_sections_callback,
    .get_num_rows = menu_get_num_rows_callback,
    .get_header_height = menu_get_header_height_callback,
    .get_cell_height = menu_get_cell_height_callback,
    .draw_header = menu_draw_header_callback,
    .draw_row = menu_draw_row_callback,
    .select_click = menu_select_callback,
  });

  menu_layer_set_click_config_onto_window(s_menu_layer, window);
  layer_add_child(root_layer, menu_layer_get_layer(s_menu_layer));
}

static void window_unload(Window *window) {
  if (s_menu_layer) {
    menu_layer_destroy(s_menu_layer);
    s_menu_layer = NULL;
  }
}

void ui_menu_init(void) {
  if (!s_window) {
    s_window = window_create();
    window_set_window_handlers(s_window, (WindowHandlers){
      .load = window_load,
      .unload = window_unload
    });
  }
  bool animated = (launch_reason() != APP_LAUNCH_QUICK_LAUNCH);
  window_stack_push(s_window, animated);
}

void ui_menu_deinit(void) {
  if (s_window) {
    window_destroy(s_window);
    s_window = NULL;
  }
}

Window* ui_menu_get_window(void) {
  return s_window;
}
