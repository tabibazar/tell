#ifndef CLAUDE_MSG_H
#define CLAUDE_MSG_H

/*
 * The Messages API request stan-claw sends and the reply it reads. With an
 * MCP URL the request carries Anthropic's MCP connector (mcp_servers +
 * an mcp_toolset tool; the caller adds the anthropic-beta header). The reply's
 * text blocks become the answer; mcp_tool_use names are collected for the
 * screen. Pure: host_tests/test_sc_claude.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define CLAUDE_TURNS 7
#define CLAUDE_SYSTEM "You are stan-claw, a voice assistant on a small screen, talking with older adults. " \
    "Speak warmly and patiently in plain words. Answer in one to three short spoken sentences, one step at a time. " \
    "Say numbers, dates and times clearly (for example \"seven in the evening\"), explain any jargon simply, " \
    "and gladly repeat when asked. Before doing anything on their behalf (a payment, a booking, a request, an appeal), " \
    "say exactly what you will do and ask them to confirm. When it helps, end by offering the next step. " \
    "When the person says they are finished (for example \"that's all\", \"no thanks\", \"goodbye\"), " \
    "reply with a brief goodbye and end your reply with [END]. " \
    "When you learn something lasting about the person (where they live, their email, how they like answers), " \
    "add [NOTE: the fact] at the end of your reply; notes are saved and never spoken. " \
    "When you ask the person to confirm an action (a payment, a booking, a request, an appeal, a message to family), " \
    "end that reply with [CONFIRM: a short summary of the action, for example \"Pay $295 toward your dues\"]; " \
    "it is shown with Yes and No buttons and never spoken. Never pay or send money to anyone the HOA service does not list, " \
    "and if the person mentions a call, text or email asking them to pay urgently or by gift card, gently warn them it may be " \
    "a scam and suggest they check with family or the management office first. When the person asks you to tell a family " \
    "member something, use the notify_family tool. Payments over $100 wait for a family member's OK: explain that kindly."
/* Added for a homeowner who speaks another language; each %s its English name, "Spanish". */
#define CLAUDE_LANGUAGE "Speak and write in %s; HOA records come in English: translate them simply. " \
    "Keep the markers [END], [NOTE: ...] and [CONFIRM: ...] exactly as written, in English, but write what goes " \
    "inside [CONFIRM: ...] in %s, since it is shown to the person; notes may be in English. Say numbers, dates and " \
    "money the way a %s speaker says them."

typedef struct {
    char user[600];
    char assistant[1500];
} claude_pair_t;

typedef struct {
    claude_pair_t pair[CLAUDE_TURNS];
    int n;
} claude_history_t;

typedef struct {
    char text[1500];       /* the answer: the text blocks after the last remote tool call, joined by a space */
    char tools[160];       /* "search, fetch": remote tools used, each once */
    bool mcp_error;        /* a remote tool call failed */
    char error[160];       /* why claude_parse returned false */
    bool end;              /* Claude ended the conversation ([END], taken out of text) */
    char confirm[160];     /* the action to confirm ([CONFIRM: ...], taken out of text), for the Yes/No card */
} claude_reply_t;

#define FACTS_MAX 30
#define FACT_LEN  120

/* What stan-claw remembers about the person, oldest first. */
typedef struct {
    char fact[FACTS_MAX][FACT_LEN];
    int n;
} claude_facts_t;

void claude_history_add(claude_history_t *h, const char *user, const char *assistant);
void claude_history_clear(claude_history_t *h);
/* facts: what is remembered about the person, sent with the system prompt
   ("" or NULL for nothing). language: the person's language by its English
   name ("Spanish"); NULL, "" or "English" adds nothing, so the system prompt
   is CLAUDE_SYSTEM (and the facts) as before. */
char *claude_request(const char *model, const char *mcp_url, const char *mcp_token,
                     const claude_history_t *h, const char *facts, const char *question, const char *language);
bool claude_parse(const char *json, claude_reply_t *out);

/* Takes Claude's [END] marker out of `text` (the conversation is over);
   true if it was there. */
bool claude_take_end(char *text);

/* Takes the [CONFIRM: ...] marker out of `text`; the summary (trimmed, at
   most n-1 chars) into `summary` ("" when there was none). True if there
   was one. Tolerates "[CONFIRM:x]", extra spaces, and a missing "]" at the
   end of text. After taking it out, tidies the text's trailing spaces (as
   claude_take_end does). */
bool claude_take_confirm(char *text, char *summary, size_t n);

/* Finds a homeowner token in a raw Messages API reply (the sign_in tool's
   result text inside an mcp_tool_result block reads
   "... homeowner_token: hw_<24 hex>"). Copies "hw_..." into out. True if
   found. Works on the raw JSON string (no parsing needed); takes the LAST
   one if several. */
bool claude_find_homeowner_token(const char *raw_json, char *out, size_t n);

/* The person's own words sound like a sign-off: "that's all", "bye",
   "no thanks, I'm done" -- or in the other languages (i18n.h), with or
   without their accents: "eso es todo", "merci, au revoir", "danke,
   tschüss", "salamat, paalam". A fallback for a reply without [END]. */
bool claude_is_signoff(const char *heard);

/* Takes every [NOTE: ...] out of `text` and adds each to `f` (skipping one
   already known, ignoring case; dropping the oldest when full). Returns how
   many notes there were. */
int claude_take_notes(char *text, claude_facts_t *f);

/* The facts as one line for the system prompt, "a; b; c", into out. */
void claude_facts_line(const claude_facts_t *f, char *out, size_t n);

/* The facts as text, one per line, and back. */
void claude_facts_text(const claude_facts_t *f, char *out, size_t n);
void claude_facts_parse(const char *text, claude_facts_t *f);

/* A conversation, with the day it belongs to, as JSON (malloc'd) and back.
   claude_history_parse returns false for anything unreadable. */
char *claude_history_json(const claude_history_t *h, int day);
bool claude_history_parse(const char *json, claude_history_t *h, int *day);

/* The "day" a moment belongs to: the date of (t - 4 h), as yyyymmdd from a
   local broken-down time, so a conversation lasts until 04:00 the next day. */
int claude_day(time_t t);
/* A refusal because of the remote MCP server: a 4xx whose error names it
   (an unreachable server is a 400, "mcp_servers[0] 'remote': Error while
   communicating with MCP server."). The caller asks again without it. */
bool claude_mcp_failed(int code, const claude_reply_t *r);

#endif /* CLAUDE_MSG_H */
