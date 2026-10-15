#ifndef PORTAL_H
#define PORTAL_H
/*
 * The WiFi setup pages, while setup runs: on the setup network (192.168.4.1)
 * every name a phone looks up answers with this board (a small DNS server),
 * and every page it asks for leads to the setup page, so the phone opens it
 * by itself as a hotel's sign-in page. They share mcpd's HTTP server.
 * on_save: a network and password sent from the page (on the HTTP task).
 */
void portal_start(void (*on_save)(const char *ssid, const char *pass));
void portal_stop(void);
#endif /* PORTAL_H */
