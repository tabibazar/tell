/* stan-claw's WiFi setup page: built, its network names escaped, its form read. */
#include "setup_web.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    static char page[16384];
    const char *nets[] = { "Tabriz", "Bob's <WiFi> & \"guest\"", "" };
    int n = setup_page(page, sizeof page, nets, 3);
    CHECK(n > 0 && (size_t)n == strlen(page));
    CHECK(strstr(page, "value='Tabriz' checked>Tabriz</label>"));
    CHECK(strstr(page, "Bob&#39;s &lt;WiFi&gt; &amp; &quot;guest&quot;") && !strstr(page, "<WiFi>"));
    CHECK(strstr(page, "name='other'") && strstr(page, "name='pass'") && strstr(page, "action='/save'"));
    CHECK(setup_page(page, 200, nets, 2) == -1);                      /* too small: refused, not cut */
    n = setup_page(page, sizeof page, NULL, 0);
    CHECK(n > 0 && strstr(page, "\x01other' checked>"));              /* nothing in sight: type one */
    CHECK(setup_saved_page(page, sizeof page, "<b>x") > 0 && strstr(page, "Connecting to &lt;b&gt;x"));

    char ssid[33], pass[64];
    CHECK(setup_form("ssid=Tabriz&other=&pass=secret123", ssid, sizeof ssid, pass, sizeof pass));
    CHECK(strcmp(ssid, "Tabriz") == 0 && strcmp(pass, "secret123") == 0);
    CHECK(setup_form("ssid=Bob%27s+WiFi&pass=a%26b%3Dc%25d+e", ssid, sizeof ssid, pass, sizeof pass));
    CHECK(strcmp(ssid, "Bob's WiFi") == 0 && strcmp(pass, "a&b=c%d e") == 0);
    CHECK(setup_form("ssid=%01other&other=+Cottage+5G+&pass=", ssid, sizeof ssid, pass, sizeof pass));
    CHECK(strcmp(ssid, "Cottage 5G") == 0 && pass[0] == 0);           /* an open network, trimmed name */
    CHECK(setup_form("other=Typed&pass=12345678", ssid, sizeof ssid, pass, sizeof pass) && strcmp(ssid, "Typed") == 0);
    CHECK(!setup_form("ssid=Tabriz&pass=short", ssid, sizeof ssid, pass, sizeof pass));            /* 1-7 is not WPA2 */
    CHECK(!setup_form("ssid=%01other&other=&pass=12345678", ssid, sizeof ssid, pass, sizeof pass)); /* no name */
    CHECK(!setup_form("ssid=123456789012345678901234567890123&pass=", ssid, sizeof ssid, pass, sizeof pass));
    CHECK(!setup_form("", ssid, sizeof ssid, pass, sizeof pass));

    char qr[200];
    setup_qr_text("stan-claw-setup", "48213907", qr, sizeof qr);
    CHECK(strcmp(qr, "WIFI:S:stan-claw-setup;T:WPA;P:48213907;;") == 0);
    setup_qr_text("a;b", "p:q", qr, sizeof qr);
    CHECK(strcmp(qr, "WIFI:S:a\\;b;T:WPA;P:p\\:q;;") == 0);

    printf(fails ? "%d FAILED\n" : "sc_setup: all passed\n", fails);
    return fails != 0;
}
