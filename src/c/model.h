#pragma once

#include <pebble.h>

// 20 notes * 168 bytes = 3360 bytes, keeping us under the 4KB per-app
// persistent storage quota (25 notes = 4200 bytes would silently fail writes).
#define MAX_NOTES 20
#define LEGACY_MAX_NOTES 25
#define NOTE_TEXT_MAX_LEN 160
#define MAX_DAY_SECTIONS 15

// Default TTL: 7 days (in seconds)
#define DEFAULT_TTL_SECONDS (7 * 24 * 3600)

typedef struct {
  uint32_t id;
  time_t timestamp;
  char text[NOTE_TEXT_MAX_LEN];
} VoiceNote;

typedef struct {
  int32_t ttl_seconds;      // 0 = never, otherwise seconds
  bool quick_launch_record; // true = start mic directly on Quick Launch
} AppSettings;

typedef struct {
  char title[32];
  int start_index; // Index in s_notes
  int count;       // Number of notes on this day
} DaySection;

typedef void (*ModelChangeCallback)(void);

void model_init(void);
void model_save(void);
void model_set_change_callback(ModelChangeCallback callback);
void model_notify(void);

int model_get_note_count(void);
const VoiceNote* model_get_note(int index);
bool model_add_note(uint32_t id, time_t timestamp, const char *text);
bool model_delete_note(uint32_t id);

void model_sort_silent(void);
// Atomically replaces the full note list (used after a complete phone sync)
void model_replace_all(const VoiceNote *notes, int count);

int model_get_day_section_count(void);
const DaySection* model_get_day_section(int section_index);
const VoiceNote* model_get_note_by_section_and_row(int section_index, int row);
int model_get_global_index(int section_index, int row);

int model_cleanup_ttl(void);

const AppSettings* model_get_settings(void);
void model_update_settings(int32_t ttl_seconds, bool quick_launch_record);
