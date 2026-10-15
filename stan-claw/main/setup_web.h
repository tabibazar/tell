#ifndef SETUP_WEB_H
#define SETUP_WEB_H

/*
 * The WiFi setup page a phone opens when it joins stan-claw-setup (the QR
 * code on the screen): the networks in sight to pick from, or a name to
 * type, and a password; and reading what it sends back. Network names are
 * strangers' words: escaped on the page. Pure: host_tests/test_sc_setup.c.
 */
#include <stdbool.h>
#include <stddef.h>

/* The page, the networks in sight first (strongest first, as given). Returns
   its length, or -1 if it did not fit. */
int setup_page(char *out, size_t n, const char *const *ssids, int count);

/* The page after the phone has sent a network. */
int setup_saved_page(char *out, size_t n, const char *ssid);

/* An application/x-www-form-urlencoded body: "ssid" (or "other" when ssid is
   the "other" choice) and "pass", url-decoded. False without a name, or with
   a name over 32 bytes or a password over 63 or of 1-7 (WPA2 wants 8+). */
bool setup_form(const char *body, char *ssid, size_t ns, char *pass, size_t np);

/* The QR code's text that joins a phone to the setup network, Wi-Fi's own
   format: WIFI:S:<name>;T:WPA;P:<password>;; with ; , : " \ escaped. */
void setup_qr_text(const char *ssid, const char *pass, char *out, size_t n);

#endif /* SETUP_WEB_H */
