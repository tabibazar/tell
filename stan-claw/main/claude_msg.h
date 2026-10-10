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
    "add [NOTE: the fact] at the end of your reply; notes are saved and never spoken."

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
   ("" or NULL for nothing). */
char *claude_request(const char *model, const char *mcp_url, const char *mcp_token,
                     const claude_history_t *h, const char *facts, const char *question);
bool claude_parse(const char *json, claude_reply_t *out);

/* Takes Claude's [END] marker out of `text` (the conversation is over);
   true if it was there. */
bool claude_take_end(char *text);

/* The person's own words sound like a sign-off: "that's all", "bye",
   "no thanks, I'm done". A fallback for a reply without [END]. */
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
