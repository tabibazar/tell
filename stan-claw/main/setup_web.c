#include "setup_web.h"

#include <stdio.h>
#include <string.h>

typedef struct { char *p; size_t left; bool ok; } buf_t;

static void put(buf_t *b, const char *s)
{
    size_t l = strlen(s);
    if (l >= b->left) { b->ok = false; b->left = 0; return; }
    memcpy(b->p, s, l);
    b->p += l;
    b->left -= l;
    *b->p = 0;
}

static void put_esc(buf_t *b, const char *s)
{
    char one[2] = { 0, 0 };
    for (; *s; s++) {
        switch (*s) {
        case '<': put(b, "&lt;"); break;
        case '>': put(b, "&gt;"); break;
        case '&': put(b, "&amp;"); break;
        case '"': put(b, "&quot;"); break;
        case '\'': put(b, "&#39;"); break;
        default: one[0] = *s; put(b, one);
        }
    }
}

static const char HEAD[] =
    "<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>stan-claw WiFi</title><style>body{font-family:-apple-system,Segoe UI,Arial,sans-serif;background:#0e1014;"
    "color:#f2f4f7;margin:0;padding:22px;font-size:19px}h1{font-size:26px;margin:0 0 6px}p{color:#aab3c2;line-height:1.45}"
    "label.n{display:block;background:#1c2028;border-radius:12px;padding:16px;margin:10px 0;font-size:21px}"
    "input[type=radio]{transform:scale(1.5);margin-right:14px}input[type=text],input[type=password]{width:100%;"
    "box-sizing:border-box;font-size:21px;padding:14px;border-radius:10px;border:1px solid #3a4150;background:#151820;"
    "color:#fff;margin:8px 0 16px}button{width:100%;font-size:22px;padding:18px;border:0;border-radius:12px;"
    "background:#3b6fd0;color:#fff;font-weight:600}.s{font-size:15px;color:#8a929e}</style></head><body>";

int setup_page(char *out, size_t n, const char *const *ssids, int count)
{
    buf_t b = { out, n, true };
    if (n) out[0] = 0;
    put(&b, HEAD);
    put(&b, "<h1>Connect stan-claw to WiFi</h1><p>Choose your home network, type its password, and press Connect. "
            "The screen says when it is on.</p><form method='post' action='/save'>");
    for (int i = 0; i < count; i++) {
        if (!ssids[i] || !ssids[i][0]) continue;
        put(&b, "<label class='n'><input type='radio' name='ssid' value='");
        put_esc(&b, ssids[i]);
        put(&b, i == 0 ? "' checked>" : "'>");
        put_esc(&b, ssids[i]);
        put(&b, "</label>");
    }
    put(&b, "<label class='n'><input type='radio' name='ssid' value='\x01other'");
    put(&b, count ? ">" : " checked>");
    put(&b, "Another network: <input type='text' name='other' maxlength='32' autocapitalize='off' autocorrect='off' "
            "placeholder='its name'></label><label>Password</label><input type='password' name='pass' id='pw' "
            "maxlength='63' autocapitalize='off' autocorrect='off'><label class='s'><input type='checkbox' "
            "onclick=\"document.getElementById('pw').type=this.checked?'text':'password'\"> show the password</label>"
            "<p></p><button type='submit'>Connect</button></form></body></html>");
    return b.ok ? (int)(n - b.left) : -1;
}

int setup_saved_page(char *out, size_t n, const char *ssid)
{
    buf_t b = { out, n, true };
    if (n) out[0] = 0;
    put(&b, HEAD);
    put(&b, "<h1>Connecting to ");
    put_esc(&b, ssid);
    put(&b, "</h1><p>Look at the stan-claw screen: it says when it is connected, in about half a minute. If the "
            "password was wrong it says so, and you can try again from this page.</p></body></html>");
    return b.ok ? (int)(n - b.left) : -1;
}

static int hex(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* The value of `key` in a urlencoded body, decoded into out; false if absent or too long. */
static bool field(const char *body, const char *key, char *out, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = body; p && *p; p = strchr(p, '&'), p = p ? p + 1 : NULL) {
        if (strncmp(p, key, kl) != 0 || p[kl] != '=') continue;
        size_t o = 0;
        for (const char *q = p + kl + 1; *q && *q != '&'; q++) {
            int c = (unsigned char)*q;
            if (c == '+') c = ' ';
            else if (c == '%' && hex(q[1]) >= 0 && hex(q[2]) >= 0) { c = hex(q[1]) * 16 + hex(q[2]); q += 2; }
            if (o + 1 >= n) return false;
            out[o++] = (char)c;
        }
        out[o] = 0;
        return true;
    }
    return false;
}

bool setup_form(const char *body, char *ssid, size_t ns, char *pass, size_t np)
{
    char pick[40] = "", other[40] = "";
    ssid[0] = pass[0] = 0;
    if (!field(body, "ssid", pick, sizeof pick)) pick[0] = '\x01';      /* nothing picked: the typed one */
    const char *name = pick;
    if (pick[0] == '\x01') {
        if (!field(body, "other", other, sizeof other)) return false;
        name = other;
    }
    while (*name == ' ') name++;
    size_t l = strlen(name);
    while (l && name[l - 1] == ' ') l--;
    if (l == 0 || l > 32 || l + 1 > ns) return false;
    memcpy(ssid, name, l);
    ssid[l] = 0;
    char pw[80] = "";
    if (!field(body, "pass", pw, sizeof pw)) pw[0] = 0;
    size_t pl = strlen(pw);
    if (pl > 63 || (pl > 0 && pl < 8) || pl + 1 > np) return false;
    memcpy(pass, pw, pl + 1);
    return true;
}

void setup_qr_text(const char *ssid, const char *pass, char *out, size_t n)
{
    char es[80], ep[140];
    size_t o = 0;
    for (const char *s = ssid; *s && o + 2 < sizeof es; s++) {
        if (strchr(";,:\"\\", *s)) es[o++] = '\\';
        es[o++] = *s;
    }
    es[o] = 0;
    o = 0;
    for (const char *s = pass; *s && o + 2 < sizeof ep; s++) {
        if (strchr(";,:\"\\", *s)) ep[o++] = '\\';
        ep[o++] = *s;
    }
    ep[o] = 0;
    snprintf(out, n, "WIFI:S:%s;T:WPA;P:%s;;", es, ep);
}
