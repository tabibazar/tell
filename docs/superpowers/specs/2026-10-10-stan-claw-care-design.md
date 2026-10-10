# stan-claw care features: for older users and the people who look out for them

Date: 2026-10-10. Status: requested in full by Reza ("build them all"); decisions below are his. Built 2026-10-10.

## Decisions

| Question | Decision |
|---|---|
| Family channel | Email via AWS SES, from `stan-claw@avriz.io` (avriz.io is a verified SES domain; the account has production access) |
| Family contact for the demo | Reza (address in `FAMILY_EMAIL`, gitignored `secrets/stan-mcp.env`, and the Lambda's environment) |
| Payments needing a family OK | Over **$100** |
| Spoken reminders | Only between **9 am and 7 pm** local time |

## For the person using it (firmware, `stan-claw/`)

1. **Repeat / Slower / Done buttons.** In a conversation, after an answer the listening page ("Your turn") shows three large buttons: *Say it again*, *Slower please*, *I'm done*. Again replays the last answer; Slower replays it at speed 0.75; Done ends the conversation. Tapping elsewhere no longer ends it (no accidental stops). Saying "say that again" still works.
2. **Yes / No confirm card.** Claude is told: when asking the person to confirm an action, add `[CONFIRM: short summary]`. stan-claw strips the marker, shows a card (the summary, a big green **Yes**, a grey **No**) while it listens; a tap answers as if spoken ("Yes." / "No."), speaking works too.
3. **Clarity voice.** A high-shelf boost (+6 dB above ~2.5 kHz, where consonants live) on all speech, after the robot effects. Settings switch *Clarity for hearing* (on by default), NVS key `clarity`.
4. **Captions that follow the speech.** While speaking, the answer's words already spoken are bright, the rest dim, advanced by audio played against an estimate (characters / speaking rate), finished when speech ends.
5. **Knock twice to talk.** Two knocks 0.25-0.9 s apart start a conversation (from home, or from rest: it wakes and listens). One knock still only wakes.
6. **Text size and contrast.** Settings: *Text* Large / Extra large (answers in a 40 px face), *Contrast* Normal / High (yellow on black). NVS keys `textsize`, `contrast`.
7. **Friendly clock.** The resting screen says the time, then "Saturday afternoon" and "October 10" (morning 5-12, afternoon 12-17, evening 17-21, night).
8. **Gentle reminders.** stan-claw speaks reminders on its own, 9 am-7 pm only, with a soft chime and a card, each once:
   trash night ("Trash bins go out tonight; pickup is tomorrow"), meeting days, dues due in 3 days, violation deadlines in 2 days, bookings tomorrow. stan-mcp computes them (`reminders` tool); stan-claw fetches them a few times a day when awake, keeps them in flash, and wakes from deep sleep for one if it must.

## For friends and family (stan-mcp, the Lambda)

9. **"Let my daughter know."** Tool `notify_family(homeowner_token, message)`: emails the homeowner's helpers. Claude uses it when asked ("please tell my daughter...").
10. **Weekly family summary.** With the homeowner's consent (on for the demo homeowner), every Sunday at 10 am Eastern an email to helpers: what they asked about and did that week (from an activity log of their tool calls), and what needs attention (balance, deadlines, hearings, open requests). Tool `send_family_summary` sends one now (for demos); `family_summary_consent(on)` switches it.
11. **Messages from family on the screen.** Every family email carries a private link to a small web page (on the Lambda URL) where the helper types a message. stan-claw checks for messages every 2 minutes while awake; a new one gets a chime, a card ("Message from Reza: ..."), and is read aloud. Messages wait while it deep-sleeps.
12. **A second check on money.** `make_payment` over $100 does not pay: it records a pending approval, emails helpers a link to an Approve / Decline page (the GET only shows the buttons, so a mail scanner opening the link approves nothing; the POST decides), and tells Claude "waiting for Reza's OK". Approving records it, emails a confirmation and sends a screen message ("Reza approved your $300 payment"). `pending_approvals` reports status. Claude is also told: never pay anyone it does not know, and warn about phone calls asking for payment.
13. **Helper contacts.** Tools `family_contacts`, `add_family_contact(name, relation, email)`. Seed: the demo homeowner `info@avriz.io` has helper Reza, from the `FAMILY_EMAIL` setting (never in git).

## Plumbing

- **State:** one DynamoDB table `stan-mcp-demo-state` (pay per request; pk/sk strings) holding helpers, consent, activity, pending approvals and family messages. A storage layer with an in-memory twin for tests.
- **Email:** SES `send_email` from `stan-claw@avriz.io`, plain text plus simple HTML.
- **Links:** `/family?h=<id>&s=<hmac>` (message form, GET/POST) and `/approve?id=<approval>&d=yes|no&s=<hmac>`, signed with MCP_TOKEN, no bearer needed; everything else still needs the bearer token.
- **Schedule:** an EventBridge rule (weekly, Sunday 14:00 UTC = 10:00 EDT) invokes the Lambda with `{"weekly_summary": true}`.
- **IAM:** the Lambda role gets DynamoDB on that table and SES send for avriz.io.
- **The board calls stan-mcp itself** for reminders and messages: a small JSON-RPC client over the existing HTTPS helper, using the MCP URL/token, and a **homeowner token** that stan-claw learns from the `sign_in` result inside Claude's reply (`mcp_tool_result` text "homeowner_token: hw_..."), saved in NVS (`hw_token`).

## Not included

SMS; replies by email; approvals by voice; reminders outside 9-7; family access to transcripts (the summary is built from stan-mcp's own activity log, not the board's conversations).
