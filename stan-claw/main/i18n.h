#ifndef I18N_H
#define I18N_H

/*
 * The homeowner's language: the words stan-claw puts on the conversation's
 * screens and says on its own (the recorded phrases, the check-in, the
 * goodbye), in English, Spanish, French, Portuguese, Italian, German and
 * Tagalog. Claude answers in the language itself (claude_request's
 * language), Deepgram is told which one it hears, and ElevenLabs speaks
 * them all with the same voices. The settings pages stay English, but for
 * the picker, where each language is written in its own name.
 *
 * Strings are UTF-8; stan-claw's faces (fonts.c) carry the accented letters
 * these languages use. Every translation of a format keeps the English one's
 * single %s. A missing translation falls back to English.
 * Pure: host_tests/test_sc_i18n.c.
 */
#include <stddef.h>

typedef enum { LANG_EN, LANG_ES, LANG_FR, LANG_PT, LANG_IT, LANG_DE, LANG_TL, LANG_N } lang_t;

int lang_find(const char *code);       /* "es" -> LANG_ES; "" -> LANG_EN; anything else -1 */
int lang_from_code(const char *code);  /* as lang_find, but English for anything unknown */
const char *lang_code(int lang);       /* "es" */
const char *lang_name(int lang);       /* in its own words: "Español" */
const char *lang_english(int lang);    /* for Claude: "Spanish" */

typedef enum {
    /* home */
    TR_TALK, TR_TALK_HINT, TR_NO_WIFI,
    /* listening, the conversation's buttons, the Yes/No card */
    TR_BANNER, TR_LISTENING, TR_YOUR_TURN, TR_HOW_ARE_YOU, TR_PAUSE_HINT, TR_KEEP_TALKING,
    TR_AGAIN, TR_SLOWER, TR_DONE, TR_AGAIN_SUB, TR_SLOWER_SUB, TR_DONE_SUB, TR_STOP,
    TR_SHALL_I, TR_YES, TR_NO, TR_SAY_YES_NO, TR_THINKING,
    /* cards and photos */
    TR_OK, TR_REMINDER, TR_MESSAGE, TR_A_PHOTO, TR_MSG_FROM, TR_MSG_FAMILY, TR_PHOTO_FROM, TR_PHOTO_FAMILY,
    TR_SENT_PHOTO, TR_FAMILY_SENT_PHOTO, TR_FROM,
    /* the weather at rest */
    TR_TODAY, TR_TOMORROW, TR_FEELS_LIKE,
    /* See and hear's preview */
    TR_ANSWER_PREVIEW,
    /* WiFi set up from a phone */
    TR_SETUP_TITLE, TR_YOUR_WIFI, TR_CONNECTED, TR_CONNECTING, TR_PHONE_AWAY, TR_HALF_MINUTE, TR_SETUP_DONE,
    TR_STEP1, TR_STEP2, TR_SETUP_NET, TR_STEP3, TR_COULDNT_JOIN, TR_CHECK_PASS1, TR_CHECK_PASS2,
    TR_NO_CAMERA, TR_PASSWORD, TR_THEN_OPEN,
    /* said, or noted, in a conversation */
    TR_ANYTHING_ELSE, TR_GOODBYE, TR_CHECKIN_MORNING, TR_CHECKIN_DAY, TR_ENDED, TR_MISSED,
    TR_HOA_CHECKED, TR_HOA_FAILED,
    /* the recorded phrases (phrases.h), in their order */
    TR_PH_MOMENT1, TR_PH_MOMENT2, TR_PH_MOMENT3, TR_PH_OFFLINE, TR_PH_TROUBLE, TR_PH_MISSED,
    TR_N
} tr_key_t;

const char *tr(int lang, int key);

/* weather_words(code) in the language: "Partly cloudy", "Parcialmente nublado". */
const char *tr_weather(int lang, int code);

/* The friendly day, as ui_day_words (ui.h) describes it, in the language:
   "Saturday afternoon" / "October 10", "Sábado por la tarde" / "10 de octubre",
   "Samstagnachmittag" / "10. Oktober". month 0 (unknown) -> both empty. */
void tr_day_words(int lang, int wday, int month, int mday, int minute, char *part, size_t np, char *date, size_t nd);

#endif /* I18N_H */
