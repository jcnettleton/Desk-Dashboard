// ============================================================
// Google Apps Script — Calendar Proxy for ESP32 E-Ink Dashboard
// ============================================================
//
// SETUP INSTRUCTIONS:
//
// 1. Go to https://script.google.com → New Project
// 2. Replace the contents of Code.gs with this file
// 3. Click Deploy → New deployment
// 4. Type: "Web app"
// 5. Execute as: "Me" (your Google account)
// 6. Who has access: "Anyone"
// 7. Click Deploy → copy the URL
// 8. Paste the URL into include/config.h as APPS_SCRIPT_URL,
//    appending ?token=YOUR_SECRET (use the same secret below)
//
// TEST:
//   Open the URL in a browser:
//   https://script.google.com/macros/s/DEPLOY_ID/exec?token=YOUR_SECRET
//   You should see a JSON array of today's calendar events.
//
// ============================================================

// Change this to a secret of your choosing — must match the
// ?token= value you put in config.h on the ESP32.
var SECRET_TOKEN = "CHANGE_ME_TO_SOMETHING_RANDOM";

function doGet(e) {
  // Simple token check to prevent casual abuse
  if (!e || !e.parameter || e.parameter.token !== SECRET_TOKEN) {
    return ContentService.createTextOutput(
      JSON.stringify({ error: "unauthorized" })
    ).setMimeType(ContentService.MimeType.JSON);
  }

  try {
    var cal = CalendarApp.getDefaultCalendar();
    var now = new Date();
    var startOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 0, 0, 0);
    var endOfDay   = new Date(now.getFullYear(), now.getMonth(), now.getDate(), 23, 59, 59);
    var events = cal.getEvents(startOfDay, endOfDay);

    var result = events.map(function (ev) {
      return {
        title:  ev.getTitle(),
        start:  ev.getStartTime().toISOString(),
        end:    ev.getEndTime().toISOString(),
        allDay: ev.isAllDayEvent()
      };
    });

    return ContentService.createTextOutput(
      JSON.stringify(result)
    ).setMimeType(ContentService.MimeType.JSON);

  } catch (err) {
    return ContentService.createTextOutput(
      JSON.stringify({ error: err.toString() })
    ).setMimeType(ContentService.MimeType.JSON);
  }
}
