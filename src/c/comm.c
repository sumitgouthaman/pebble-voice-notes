#include "comm.h"
#include "model.h"

// ---------------------------------------------------------------------------
// Watch -> phone bulk streaming (ACTION_WATCH_NOTES).
// Each note is sent only after the previous one is ACKed (outbox_sent),
// avoiding APP_MSG_BUSY drops. Carries NoteCount/NoteIndex so the phone knows
// when the watch's list is complete and can reconcile.
// ---------------------------------------------------------------------------
#define SEND_RETRY_DELAY_MS 400
#define SEND_MAX_RETRIES 3

static int s_send_index = -1; // -1 = no bulk send in progress
static int s_send_total = 0;
static int s_send_retries = 0;
static AppTimer *s_send_timer = NULL;

// ---------------------------------------------------------------------------
// Phone -> watch staging (ACTION_SYNC_NOTES).
// Incoming notes are staged in a heap buffer and only committed once the whole
// batch has arrived, so an interrupted sync can never leave a partial list.
// ---------------------------------------------------------------------------
static VoiceNote *s_stage = NULL;
static int s_stage_expected = 0;
static int s_stage_received = 0;

static void send_next_note(void);

static void stage_free(void) {
  if (s_stage) {
    free(s_stage);
    s_stage = NULL;
  }
  s_stage_expected = 0;
  s_stage_received = 0;
}

static bool write_bulk_message(int index, int total, const VoiceNote *note) {
  DictionaryIterator *outbox = NULL;
  AppMessageResult result = app_message_outbox_begin(&outbox);
  if (result != APP_MSG_OK || !outbox) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "outbox_begin failed: %d", (int)result);
    return false;
  }

  dict_write_int32(outbox, MESSAGE_KEY_AppKeyAction, ACTION_WATCH_NOTES);
  dict_write_int32(outbox, MESSAGE_KEY_NoteCount, total);
  dict_write_int32(outbox, MESSAGE_KEY_NoteIndex, index);
  if (note) {
    dict_write_uint32(outbox, MESSAGE_KEY_NoteId, note->id);
    dict_write_uint32(outbox, MESSAGE_KEY_NoteTime, (uint32_t)note->timestamp);
    dict_write_cstring(outbox, MESSAGE_KEY_NoteText, note->text);
  }

  return app_message_outbox_send() == APP_MSG_OK;
}

static bool write_single_note_message(const VoiceNote *note) {
  DictionaryIterator *outbox = NULL;
  AppMessageResult result = app_message_outbox_begin(&outbox);
  if (result != APP_MSG_OK || !outbox) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "outbox_begin failed: %d", (int)result);
    return false;
  }

  dict_write_int32(outbox, MESSAGE_KEY_AppKeyAction, ACTION_NEW_NOTE);
  dict_write_uint32(outbox, MESSAGE_KEY_NoteId, note->id);
  dict_write_uint32(outbox, MESSAGE_KEY_NoteTime, (uint32_t)note->timestamp);
  dict_write_cstring(outbox, MESSAGE_KEY_NoteText, note->text);

  return app_message_outbox_send() == APP_MSG_OK;
}

static void send_timer_callback(void *data) {
  s_send_timer = NULL;
  send_next_note();
}

static void schedule_retry(void) {
  if (s_send_retries >= SEND_MAX_RETRIES) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Giving up on note sync at index %d", s_send_index);
    s_send_index = -1;
    return;
  }
  s_send_retries++;
  if (!s_send_timer) {
    s_send_timer = app_timer_register(SEND_RETRY_DELAY_MS, send_timer_callback, NULL);
  }
}

static void send_next_note(void) {
  if (s_send_index < 0) {
    return;
  }

  bool ok;
  if (s_send_total == 0) {
    // Tell the phone the watch has no notes, so it can still reconcile
    ok = write_bulk_message(0, 0, NULL);
  } else {
    if (s_send_index >= s_send_total) {
      s_send_index = -1;
      return;
    }
    const VoiceNote *note = model_get_note(s_send_index);
    if (!note) {
      s_send_index = -1;
      return;
    }
    ok = write_bulk_message(s_send_index, s_send_total, note);
  }

  if (!ok) {
    schedule_retry();
  }
}

static void handle_sync_notes(DictionaryIterator *iterator) {
  Tuple *count_tuple = dict_find(iterator, MESSAGE_KEY_NoteCount);
  Tuple *index_tuple = dict_find(iterator, MESSAGE_KEY_NoteIndex);
  Tuple *id_tuple = dict_find(iterator, MESSAGE_KEY_NoteId);
  Tuple *time_tuple = dict_find(iterator, MESSAGE_KEY_NoteTime);
  Tuple *text_tuple = dict_find(iterator, MESSAGE_KEY_NoteText);

  int count = count_tuple ? count_tuple->value->int32 : 0;
  int index = index_tuple ? index_tuple->value->int32 : 0;

  if (count <= 0) {
    stage_free();
    model_replace_all(NULL, 0);
    return;
  }
  if (count > MAX_NOTES) {
    count = MAX_NOTES;
  }

  if (index == 0) {
    stage_free();
    s_stage = malloc(sizeof(VoiceNote) * count);
    if (!s_stage) {
      APP_LOG(APP_LOG_LEVEL_ERROR, "Sync staging malloc failed");
      return;
    }
    s_stage_expected = count;
  }

  // Ignore out-of-order / orphaned packets (e.g. a batch that restarted)
  if (!s_stage || index != s_stage_received || index >= s_stage_expected) {
    return;
  }

  if (id_tuple && time_tuple && text_tuple) {
    VoiceNote *note = &s_stage[s_stage_received];
    note->id = id_tuple->value->uint32;
    note->timestamp = (time_t)time_tuple->value->uint32;
    strncpy(note->text, text_tuple->value->cstring, sizeof(note->text) - 1);
    note->text[sizeof(note->text) - 1] = '\0';
    s_stage_received++;
  } else {
    // Malformed packet: abandon this batch, keep the existing list
    stage_free();
    return;
  }

  // Commit only once the complete batch has arrived
  if (s_stage_received == s_stage_expected) {
    model_replace_all(s_stage, s_stage_received);
    stage_free();
  }
}

static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  Tuple *action_tuple = dict_find(iterator, MESSAGE_KEY_AppKeyAction);
  if (!action_tuple) {
    return;
  }

  switch (action_tuple->value->int32) {
    case ACTION_SYNC_NOTES:
      handle_sync_notes(iterator);
      break;

    case ACTION_UPDATE_SETTINGS: {
      Tuple *ttl_tuple = dict_find(iterator, MESSAGE_KEY_SettingTTL);
      Tuple *ql_tuple = dict_find(iterator, MESSAGE_KEY_SettingQuickLaunchRecord);

      int32_t ttl = ttl_tuple ? ttl_tuple->value->int32 : model_get_settings()->ttl_seconds;
      uint8_t ql = ql_tuple ? (uint8_t)ql_tuple->value->int32 : model_get_settings()->quick_launch_mode;

      model_update_settings(ttl, ql);
      break;
    }

    case ACTION_REQUEST_SYNC:
      comm_send_all_notes();
      break;

    default:
      break;
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage dropped: %d", (int)reason);
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  if (s_send_index >= 0) {
    s_send_retries = 0;
    if (s_send_total == 0) {
      s_send_index = -1; // the single "empty" message was delivered
      return;
    }
    s_send_index++;
    send_next_note();
  }
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "AppMessage outbox failed: %d", (int)reason);
  if (s_send_index < 0) {
    // Single-note send failed; fall back to full sync with retries
    s_send_retries = 0;
    s_send_total = model_get_note_count();
    s_send_index = 0;
  }
  schedule_retry();
}

void comm_init(void) {
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_sent(outbox_sent_callback);
  app_message_register_outbox_failed(outbox_failed_callback);

  // Allocate 512 bytes for inbox and outbox to comfortably hold note text
  app_message_open(512, 512);
}

void comm_deinit(void) {
  if (s_send_timer) {
    app_timer_cancel(s_send_timer);
    s_send_timer = NULL;
  }
  s_send_index = -1;
  stage_free();
  app_message_deregister_callbacks();
}

void comm_send_all_notes(void) {
  if (s_send_timer) {
    app_timer_cancel(s_send_timer);
    s_send_timer = NULL;
  }
  s_send_retries = 0;
  s_send_total = model_get_note_count();
  s_send_index = 0;
  send_next_note();
}

void comm_send_new_note(const VoiceNote *note) {
  if (!note) {
    return;
  }
  if (s_send_index >= 0 || !write_single_note_message(note)) {
    // Bulk sync in flight or outbox busy; queue a full sync with retries
    comm_send_all_notes();
  }
}

void comm_request_sync(void) {
  DictionaryIterator *outbox = NULL;
  AppMessageResult result = app_message_outbox_begin(&outbox);
  if (result == APP_MSG_OK && outbox) {
    dict_write_int32(outbox, MESSAGE_KEY_AppKeyAction, ACTION_REQUEST_SYNC);
    app_message_outbox_send();
  }
}
