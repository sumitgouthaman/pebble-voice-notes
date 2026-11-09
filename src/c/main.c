#include <pebble.h>
#include "model.h"
#include "dictation.h"
#include "ui_menu.h"
#include "ui_detail.h"
#include "comm.h"

static AppTimer *s_quick_launch_timer = NULL;
static int s_quick_launch_attempts = 0;

static void on_dictation_transcription(const char *transcription) {
  if (!transcription || strlen(transcription) == 0) {
    return;
  }

  time_t now = time(NULL);
  uint32_t note_id = (uint32_t)now;

  APP_LOG(APP_LOG_LEVEL_INFO, "Saving new voice note (id=%lu): %s", (unsigned long)note_id, transcription);
  if (model_add_note(note_id, now, transcription)) {
    const VoiceNote *new_note = model_get_note(0);
    comm_send_new_note(new_note);
  }
}

static void on_model_changed(void) {
  ui_menu_reload();
}

static void quick_launch_timer_callback(void *data) {
  s_quick_launch_timer = NULL;
  s_quick_launch_attempts++;
  APP_LOG(APP_LOG_LEVEL_INFO, "Quick Launch timer firing (attempt %d)", s_quick_launch_attempts);
  bool started = dictation_start();
  if (!started && s_quick_launch_attempts < 3) {
    s_quick_launch_timer = app_timer_register(300, quick_launch_timer_callback, NULL);
  }
}

static void init(void) {
  AppLaunchReason reason = launch_reason();
  uint32_t args = launch_get_args();
  APP_LOG(APP_LOG_LEVEL_INFO, "Voice Notes init: launch_reason=%d (QUICK_LAUNCH=%d), launch_args=%lu",
          (int)reason, (int)APP_LAUNCH_QUICK_LAUNCH, (unsigned long)args);

  // 1. Initialize data model and flash storage
  model_init();
  model_set_change_callback(on_model_changed);

  // 2. Initialize AppMessage communication
  // (PKJS sends ACTION_REQUEST_SYNC on 'ready', which triggers comm_send_all_notes)
  comm_init();

  // 3. Initialize Dictation API
  dictation_init(on_dictation_transcription);

  // 4. Initialize UI Windows
  ui_detail_init();
  ui_menu_init();

  // 5. Check if launched via Quick Launch shortcut
  const AppSettings *settings = model_get_settings();
  APP_LOG(APP_LOG_LEVEL_INFO, "Settings: ttl=%ld, quick_launch_record=%d",
          (long)(settings ? settings->ttl_seconds : -1),
          settings ? (int)settings->quick_launch_record : -1);

  if (reason == APP_LAUNCH_QUICK_LAUNCH && (!settings || settings->quick_launch_record)) {
    APP_LOG(APP_LOG_LEVEL_INFO, "Scheduling Quick Launch dictation start via AppTimer");
    s_quick_launch_attempts = 0;
    s_quick_launch_timer = app_timer_register(200, quick_launch_timer_callback, NULL);
  }
}

static void deinit(void) {
  if (s_quick_launch_timer) {
    app_timer_cancel(s_quick_launch_timer);
    s_quick_launch_timer = NULL;
  }
  ui_menu_deinit();
  ui_detail_deinit();
  dictation_deinit();
  comm_deinit();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
