#pragma once

#include <pebble.h>
#include "model.h"

enum {
  ACTION_NEW_NOTE = 1,        // watch -> phone: single freshly recorded note
  ACTION_SYNC_NOTES = 2,      // phone -> watch: authoritative note list (streamed)
  ACTION_UPDATE_SETTINGS = 3, // phone -> watch: settings
  ACTION_REQUEST_SYNC = 4,    // phone -> watch: "send me all your notes"
  ACTION_WATCH_NOTES = 5      // watch -> phone: full watch note list (streamed)
};

void comm_init(void);
void comm_deinit(void);
void comm_send_new_note(const VoiceNote *note);
void comm_send_all_notes(void);
void comm_request_sync(void);
