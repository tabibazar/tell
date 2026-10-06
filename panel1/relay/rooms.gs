/*
 * panel1's relay: one day's bookings for every meeting room this account can
 * see, as small JSON for the panel to draw.
 *
 * Paste into a new project at script.google.com (signed in as the account that
 * sees the rooms), then Deploy > New deployment > Web app, "Execute as: Me",
 * "Who has access: Anyone". The /exec URL it gives goes in
 * secrets/panel1.env as RELAY_URL=... -- it is the only key, so keep it there.
 *
 *   GET <url>?d=2026-10-07   (no d: today)
 *
 * {"d":"2026-10-07","tz":"America/Toronto","rooms":[
 *   {"n":"CEDAR (8)","ev":[[660,720,"Design review","Lena"], ...]}, ...]}
 *
 * Times are minutes from local midnight, clipped to the day. All-day events
 * and bookings the room itself declined are left out.
 */
function doGet(e) {
  var tz = Session.getScriptTimeZone();
  var ds = (e && e.parameter && e.parameter.d) || Utilities.formatDate(new Date(), tz, 'yyyy-MM-dd');
  if (!/^\d{4}-\d{2}-\d{2}$/.test(ds)) return json({ err: 'bad date' });
  var day = Utilities.parseDate(ds + ' 00:00', tz, 'yyyy-MM-dd HH:mm');
  var next = new Date(day.getTime() + 36 * 3600 * 1000);  /* noon tomorrow, then back to midnight: DST-safe */
  next = Utilities.parseDate(Utilities.formatDate(next, tz, 'yyyy-MM-dd') + ' 00:00', tz, 'yyyy-MM-dd HH:mm');

  var rooms = CalendarApp.getAllCalendars().filter(function (c) {
    return /@resource\.calendar\.google\.com$/.test(c.getId());
  }).sort(function (a, b) { return a.getName() < b.getName() ? -1 : 1; });

  var out = rooms.map(function (cal) {
    var id = cal.getId();
    var ev = cal.getEvents(day, next).filter(function (x) {
      if (x.isAllDayEvent()) return false;
      var g = x.getGuestByEmail(id);
      return !(g && g.getGuestStatus() == CalendarApp.GuestStatus.NO);
    }).map(function (x) {
      return [mins(x.getStartTime(), day), mins(x.getEndTime(), day),
              x.getTitle() || 'Busy', organiser(x)];
    });
    return { n: cal.getName().replace(/^.*---/, ''), ev: ev };
  });
  return json({ d: ds, tz: tz, rooms: out });
}

function mins(t, day) {
  return Math.max(0, Math.min(1440, Math.round((t.getTime() - day.getTime()) / 60000)));
}

/* The creator's name as the guest list knows it, else their address's first
   part, capitalised: "enric.s@..." -> "Lena". */
function organiser(x) {
  var who = (x.getCreators() || [])[0] || '';
  var g = who && x.getGuestByEmail(who);
  var name = g && g.getName();
  if (name && name.indexOf('@') < 0) return name.split(' ')[0];
  var p = who.split('@')[0].split(/[._]/)[0];
  return p ? p.charAt(0).toUpperCase() + p.slice(1) : '';
}

function json(o) {
  return ContentService.createTextOutput(JSON.stringify(o)).setMimeType(ContentService.MimeType.JSON);
}
