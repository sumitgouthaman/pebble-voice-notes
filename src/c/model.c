#include "model.h"
#include <string.h>

#define PERSIST_KEY_COUNT     100
#define PERSIST_KEY_SETTINGS  101
#define PERSIST_KEY_NOTE_BASE 1000

static VoiceNote s_notes[MAX_NOTES];
static int s_note_count = 0;

static DaySection s_day_sections[MAX_DAY_SECTIONS];
static int s_day_section_count = 0;

static AppSettings s_settings = {
  .ttl_seconds = DEFAULT_TTL_SECONDS,
  .quick_launch_mode = QUICK_LAUNCH_NEW_NOTE
};

static ModelChangeCallback s_change_callback = NULL;

static void rebuild_day_sections(void);
static int cleanup_ttl_silent(void);

void model_set_change_callback(ModelChangeCallback callback) {
  s_change_callback = callback;
}

void model_notify(void) {
  rebuild_day_sections();
  if (s_change_callback) {
    s_change_callback();
  }
}

static void rebuild_day_sections(void) {
  s_day_section_count = 0;
  if (s_note_count == 0) {
    return;
  }

  time_t now = time(NULL);
  struct tm *now_tm = localtime(&now);
  int now_year = now_tm ? now_tm->tm_year : 0;
  int now_yday = now_tm ? now_tm->tm_yday : 0;

  int current_year = -1;
  int current_yday = -1;
  int current_section_idx = -1;

  for (int i = 0; i < s_note_count; i++) {
    time_t ts = s_notes[i].timestamp;
    struct tm *note_tm = localtime(&ts);
    if (!note_tm) {
      // Keep the row reachable even if the date can't be resolved
      if (current_section_idx >= 0) {
        s_day_sections[current_section_idx].count++;
      }
      continue;
    }

    if (note_tm->tm_year != current_year || note_tm->tm_yday != current_yday) {
      if (s_day_section_count >= MAX_DAY_SECTIONS) {
        // Exceeded max day sections, attach to last section
        if (current_section_idx >= 0) {
          s_day_sections[current_section_idx].count++;
        }
        continue;
      }

      current_section_idx = s_day_section_count;
      s_day_section_count++;

      current_year = note_tm->tm_year;
      current_yday = note_tm->tm_yday;

      DaySection *sec = &s_day_sections[current_section_idx];
      sec->start_index = i;
      sec->count = 1;

      // Determine friendly section title
      if (note_tm->tm_year == now_year && note_tm->tm_yday == now_yday) {
        snprintf(sec->title, sizeof(sec->title), "Today");
      } else if ((note_tm->tm_year == now_year && note_tm->tm_yday == now_yday - 1) ||
                 (note_tm->tm_year == now_year - 1 && now_yday == 0 && note_tm->tm_yday >= 364)) {
        snprintf(sec->title, sizeof(sec->title), "Yesterday");
      } else {
        // Standard format: Sat, Nov 07, 2026 (using %d per Gotcha E)
        strftime(sec->title, sizeof(sec->title), "%a, %b %d, %Y", note_tm);
      }
    } else {
      if (current_section_idx >= 0) {
        s_day_sections[current_section_idx].count++;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

static void save_settings(void) {
  persist_write_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(AppSettings));
}

static void save_notes(void) {
  int prev_count = persist_exists(PERSIST_KEY_COUNT) ? persist_read_int(PERSIST_KEY_COUNT) : 0;
  persist_write_int(PERSIST_KEY_COUNT, s_note_count);

  for (int i = 0; i < s_note_count; i++) {
    persist_write_data(PERSIST_KEY_NOTE_BASE + i, &s_notes[i], sizeof(VoiceNote));
  }

  // Clean up orphaned keys to prevent flash quota leak (Gotcha A)
  for (int i = s_note_count; i < prev_count; i++) {
    persist_delete(PERSIST_KEY_NOTE_BASE + i);
  }
}

void model_save(void) {
  save_settings();
  save_notes();
}

void model_init(void) {
  // Load settings
  if (persist_exists(PERSIST_KEY_SETTINGS)) {
    persist_read_data(PERSIST_KEY_SETTINGS, &s_settings, sizeof(AppSettings));
  }

  // Load notes
  s_note_count = 0;
  if (persist_exists(PERSIST_KEY_COUNT)) {
    int stored_count = persist_read_int(PERSIST_KEY_COUNT);
    if (stored_count > MAX_NOTES) {
      stored_count = MAX_NOTES;
    }

    for (int i = 0; i < stored_count; i++) {
      int key = PERSIST_KEY_NOTE_BASE + i;
      if (persist_exists(key)) {
        persist_read_data(key, &s_notes[s_note_count], sizeof(VoiceNote));
        s_notes[s_note_count].text[NOTE_TEXT_MAX_LEN - 1] = '\0';
        s_note_count++;
      }
    }
  }

  // Reclaim flash from keys beyond the current limit (older builds stored up to 25)
  for (int i = MAX_NOTES; i < LEGACY_MAX_NOTES; i++) {
    if (persist_exists(PERSIST_KEY_NOTE_BASE + i)) {
      persist_delete(PERSIST_KEY_NOTE_BASE + i);
    }
  }

  model_sort_silent();

  // Clean up any expired notes (persists only if something was removed)
  if (cleanup_ttl_silent() > 0) {
    save_notes();
  }

  // Rebuild section grouping
  rebuild_day_sections();
}

// ---------------------------------------------------------------------------
// Accessors / mutations
// ---------------------------------------------------------------------------

int model_get_note_count(void) {
  return s_note_count;
}

const VoiceNote* model_get_note(int index) {
  if (index < 0 || index >= s_note_count) {
    return NULL;
  }
  return &s_notes[index];
}

bool model_add_note(uint32_t id, time_t timestamp, const char *text) {
  if (!text || strlen(text) == 0) {
    return false;
  }

  // Shift existing notes right to insert new note at index 0 (most recent).
  // When full, the oldest note falls off the end (it remains on the phone).
  int new_count = s_note_count < MAX_NOTES ? s_note_count + 1 : MAX_NOTES;
  for (int i = new_count - 1; i > 0; i--) {
    s_notes[i] = s_notes[i - 1];
  }

  s_notes[0].id = id;
  s_notes[0].timestamp = timestamp;
  strncpy(s_notes[0].text, text, NOTE_TEXT_MAX_LEN - 1);
  s_notes[0].text[NOTE_TEXT_MAX_LEN - 1] = '\0';
  s_note_count = new_count;

  save_notes();
  model_notify();
  return true;
}

bool model_delete_note(uint32_t id) {
  int found_idx = -1;
  for (int i = 0; i < s_note_count; i++) {
    if (s_notes[i].id == id) {
      found_idx = i;
      break;
    }
  }

  if (found_idx < 0) {
    return false;
  }

  for (int i = found_idx; i < s_note_count - 1; i++) {
    s_notes[i] = s_notes[i + 1];
  }
  s_note_count--;

  save_notes();
  model_notify();
  return true;
}

void model_replace_all(const VoiceNote *notes, int count) {
  if (count < 0) count = 0;
  if (count > MAX_NOTES) count = MAX_NOTES;

  s_note_count = 0;
  for (int i = 0; i < count; i++) {
    s_notes[s_note_count] = notes[i];
    s_notes[s_note_count].text[NOTE_TEXT_MAX_LEN - 1] = '\0';
    s_note_count++;
  }

  model_sort_silent();
  cleanup_ttl_silent();
  save_notes();
  model_notify();
}

// Insertion sort, most recent first. Uses a single VoiceNote temp (168 bytes)
// so it stays well within the app stack.
void model_sort_silent(void) {
  for (int i = 1; i < s_note_count; i++) {
    if (s_notes[i].timestamp <= s_notes[i - 1].timestamp) {
      continue;
    }
    VoiceNote tmp = s_notes[i];
    int j = i - 1;
    while (j >= 0 && s_notes[j].timestamp < tmp.timestamp) {
      s_notes[j + 1] = s_notes[j];
      j--;
    }
    s_notes[j + 1] = tmp;
  }
}

int model_get_day_section_count(void) {
  return s_day_section_count;
}

const DaySection* model_get_day_section(int section_index) {
  if (section_index < 0 || section_index >= s_day_section_count) {
    return NULL;
  }
  return &s_day_sections[section_index];
}

const VoiceNote* model_get_note_by_section_and_row(int section_index, int row) {
  const DaySection *sec = model_get_day_section(section_index);
  if (!sec || row < 0 || row >= sec->count) {
    return NULL;
  }
  int global_idx = sec->start_index + row;
  return model_get_note(global_idx);
}

int model_get_global_index(int section_index, int row) {
  const DaySection *sec = model_get_day_section(section_index);
  if (!sec || row < 0 || row >= sec->count) {
    return -1;
  }
  return sec->start_index + row;
}

// Removes expired notes from RAM only. Callers decide when to persist.
static int cleanup_ttl_silent(void) {
  if (s_settings.ttl_seconds <= 0 || s_note_count == 0) {
    return 0;
  }

  time_t now = time(NULL);
  int kept = 0;

  // Compact in place. NOTE: never allocate a VoiceNote array on the stack here;
  // MAX_NOTES * sizeof(VoiceNote) is several KB and overflows the app stack,
  // crashing the app on every launch once notes exist.
  for (int i = 0; i < s_note_count; i++) {
    time_t age = now - s_notes[i].timestamp;
    if (age <= s_settings.ttl_seconds) {
      if (kept != i) {
        s_notes[kept] = s_notes[i];
      }
      kept++;
    }
  }

  int removed = s_note_count - kept;
  s_note_count = kept;
  return removed;
}

int model_cleanup_ttl(void) {
  int removed = cleanup_ttl_silent();
  if (removed > 0) {
    save_notes();
    rebuild_day_sections();
  }
  return removed;
}

const AppSettings* model_get_settings(void) {
  return &s_settings;
}

void model_update_settings(int32_t ttl_seconds, uint8_t quick_launch_mode) {
  bool changed = (s_settings.ttl_seconds != ttl_seconds) ||
                 (s_settings.quick_launch_mode != quick_launch_mode);
  s_settings.ttl_seconds = ttl_seconds;
  s_settings.quick_launch_mode = quick_launch_mode;
  if (changed) {
    save_settings();
  }
  if (cleanup_ttl_silent() > 0) {
    save_notes();
  }
  model_notify();
}
