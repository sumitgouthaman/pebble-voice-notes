#include "dictation.h"
#include "model.h"

#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation_session = NULL;
static DictationNoteCallback s_callback = NULL;

static void dictation_status_callback(DictationSession *session,
                                      DictationSessionStatus status,
                                      char *transcription,
                                      void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Dictation callback status=%d text='%s'",
          (int)status, (status == DictationSessionStatusSuccess && transcription) ? transcription : "");
  if (status == DictationSessionStatusSuccess) {
    if (s_callback && transcription && strlen(transcription) > 0) {
      // Haptic confirmation of successful capture
      vibes_short_pulse();
      s_callback(transcription);
    }
  } else {
    APP_LOG(APP_LOG_LEVEL_WARNING, "Dictation session ended with non-success status: %d", (int)status);
  }
}
#endif

void dictation_init(DictationNoteCallback callback) {
#if defined(PBL_MICROPHONE)
  s_callback = callback;
  if (!s_dictation_session) {
    s_dictation_session = dictation_session_create(NOTE_TEXT_MAX_LEN, dictation_status_callback, NULL);
    APP_LOG(APP_LOG_LEVEL_INFO, "dictation_session_create: %p", (void *)s_dictation_session);
    if (s_dictation_session) {
      dictation_session_enable_confirmation(s_dictation_session, true);
      dictation_session_enable_error_dialogs(s_dictation_session, true);
    }
  }
#else
  (void)callback;
  APP_LOG(APP_LOG_LEVEL_WARNING, "PBL_MICROPHONE not defined on this platform");
#endif
}

void dictation_deinit(void) {
#if defined(PBL_MICROPHONE)
  if (s_dictation_session) {
    dictation_session_destroy(s_dictation_session);
    s_dictation_session = NULL;
  }
  s_callback = NULL;
#endif
}

bool dictation_start(void) {
#if defined(PBL_MICROPHONE)
  if (!s_dictation_session) {
    dictation_init(s_callback);
  }
  if (s_dictation_session) {
    DictationSessionStatus status = dictation_session_start(s_dictation_session);
    APP_LOG(APP_LOG_LEVEL_INFO, "dictation_session_start returned status=%d", (int)status);
    return (status == DictationSessionStatusSuccess);
  }
  APP_LOG(APP_LOG_LEVEL_ERROR, "dictation_start failed: s_dictation_session is NULL");
  return false;
#else
  APP_LOG(APP_LOG_LEVEL_WARNING, "dictation_start called on platform without microphone");
  return false;
#endif
}

bool dictation_is_supported(void) {
#if defined(PBL_MICROPHONE)
  return true;
#else
  return false;
#endif
}
