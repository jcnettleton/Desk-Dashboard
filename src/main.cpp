// ============================================================
// ESP32-S3 + Waveshare 5.79" E-Paper — Calendar Dashboard
// ============================================================
// Board:  ESP32-S3-WROOM-1-N8R8
// Panel:  GDEY0579T93, 792×272, B/W, dual SSD1683
// Orient: Portrait (272×792) — stood up vertically
// ============================================================

#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <GxEPD2_BW.h>
#include <time.h>

// Adafruit GFX FreeFonts
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

// User config (WiFi creds, Apps Script URL, timezone, rotation)
#include "config.h"

// ---- Hardware pin definitions (Waveshare 5.79" ESP32-S3) ----
#define EPD_SCK    12
#define EPD_MOSI   11
#define EPD_CS     45
#define EPD_DC     46
#define EPD_RST    47
#define EPD_BUSY   48
#define EPD_POWER   7   // Must be HIGH before display init

// ---- Display constructor ----
GxEPD2_BW<GxEPD2_579_GDEY0579T93, GxEPD2_579_GDEY0579T93::HEIGHT>
    display(GxEPD2_579_GDEY0579T93(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ---- Layout constants (portrait: 272 wide × 792 tall) ----
static const int SCREEN_W          = 272;
static const int SCREEN_H          = 792;

static const int HEADER_H          = 42;     // Date header
static const int FOOTER_H          = 18;     // "Updated HH:MM" bar
static const int TIMELINE_Y        = HEADER_H;
static const int TIMELINE_H        = SCREEN_H - HEADER_H - FOOTER_H; // 732px
static const int FOOTER_Y          = SCREEN_H - FOOTER_H;

static const int HOUR_START        = 9;      // 9 AM
static const int HOUR_END          = 17;     // 5 PM
static const int TOTAL_MINUTES     = (HOUR_END - HOUR_START) * 60;  // 480

static const int LABEL_W           = 38;     // Width of hour-label gutter
static const int EVENT_X           = LABEL_W + 2;
static const int EVENT_W           = SCREEN_W - EVENT_X - 4; // ~228px

// ---- Calendar event storage ----
struct CalEvent {
  char title[60];
  int  startHour, startMin;
  int  endHour,   endMin;
  bool allDay;
};

static const int MAX_EVENTS = 20;
CalEvent events[MAX_EVENTS];
int      eventCount = 0;

// ---- Timing intervals (ms) ----
static const unsigned long NOW_LINE_INTERVAL   = 2UL * 60 * 1000;   // 2 min
static const unsigned long FETCH_INTERVAL      = 10UL * 60 * 1000;  // 10 min
static const unsigned long FULL_REFRESH_INTERVAL = 60UL * 60 * 1000; // 1 hour

unsigned long lastNowLineUpdate  = 0;
unsigned long lastFetch          = 0;
unsigned long lastFullRefresh    = 0;
int           prevNowLineY       = -1;

// ============================================================
// WiFi
// ============================================================

void connectWiFi()
{
  Serial.printf("Connecting to WiFi \"%s\"...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected!  IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi connection FAILED — calendar won't update.");
  }
}

// ============================================================
// NTP time sync
// ============================================================

void syncTime()
{
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", TIMEZONE, 1);
  tzset();

  Serial.print("Waiting for NTP sync...");
  struct tm t;
  int retries = 0;
  while (!getLocalTime(&t) && retries < 20) {
    delay(500);
    Serial.print(".");
    retries++;
  }
  if (retries < 20) {
    char buf[40];
    strftime(buf, sizeof(buf), "%A, %B %d %Y  %H:%M:%S", &t);
    Serial.printf("\nTime synced: %s\n", buf);
  } else {
    Serial.println("\nNTP sync failed!");
  }
}

// ============================================================
// iCal stream parser — helpers
// ============================================================

// Convert a UTC datetime to local time using the configured timezone.
void utcToLocal(int &year, int &month, int &day, int &hour, int &minute)
{
  struct tm utcTm;
  memset(&utcTm, 0, sizeof(utcTm));
  utcTm.tm_year  = year - 1900;
  utcTm.tm_mon   = month - 1;
  utcTm.tm_mday  = day;
  utcTm.tm_hour  = hour;
  utcTm.tm_min   = minute;
  utcTm.tm_isdst = 0;

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(&utcTm);

  setenv("TZ", TIMEZONE, 1);
  tzset();

  struct tm loc;
  localtime_r(&epoch, &loc);
  year   = loc.tm_year + 1900;
  month  = loc.tm_mon + 1;
  day    = loc.tm_mday;
  hour   = loc.tm_hour;
  minute = loc.tm_min;
}

// Parsed datetime from an iCal value.
struct ICalDT {
  int  year, month, day, hour, min;
  bool dateOnly;
};

// Parse "20260414T090000", "20260414T150000Z", or "20260414".
ICalDT parseICalDT(const char* val)
{
  ICalDT dt = {0, 0, 0, 0, 0, false};
  int len = strlen(val);
  if (len < 8) return dt;

  sscanf(val, "%4d%2d%2d", &dt.year, &dt.month, &dt.day);
  if (len == 8) { dt.dateOnly = true; return dt; }

  if (len >= 15) sscanf(val + 9, "%2d%2d", &dt.hour, &dt.min);

  if (val[len - 1] == 'Z')
    utcToLocal(dt.year, dt.month, dt.day, dt.hour, dt.min);

  return dt;
}

// Parse DURATION value like "PT1H30M" into hours and minutes.
void parseDuration(const char* dur, int &hours, int &minutes)
{
  hours = 0; minutes = 0;
  const char* p = dur;
  if (*p == 'P') p++;
  if (*p == 'T') p++;
  int num = 0;
  while (*p) {
    if (*p >= '0' && *p <= '9') { num = num * 10 + (*p - '0'); }
    else if (*p == 'H') { hours = num; num = 0; }
    else if (*p == 'M') { minutes = num; num = 0; }
    else num = 0;
    p++;
  }
}

// Convert a YYYYMMDD string to a time_t (midnight UTC) for date arithmetic.
time_t ymdToEpoch(const char* ymd)
{
  struct tm t;
  memset(&t, 0, sizeof(t));
  int y, m, d;
  sscanf(ymd, "%4d%2d%2d", &y, &m, &d);
  t.tm_year = y - 1900;
  t.tm_mon  = m - 1;
  t.tm_mday = d;
  // Use UTC to avoid DST issues in day counting
  setenv("TZ", "UTC0", 1);  tzset();
  time_t e = mktime(&t);
  setenv("TZ", TIMEZONE, 1); tzset();
  return e;
}

// Count whole days between two YYYYMMDD strings.
int daysBetweenYMD(const char* from, const char* to)
{
  time_t a = ymdToEpoch(from);
  time_t b = ymdToEpoch(to);
  return (int)((b - a) / 86400);
}

// Count months between two YYYYMMDD strings.
int monthsBetweenYMD(const char* from, const char* to)
{
  int y1, m1, d1, y2, m2, d2;
  sscanf(from, "%4d%2d%2d", &y1, &m1, &d1);
  sscanf(to,   "%4d%2d%2d", &y2, &m2, &d2);
  return (y2 - y1) * 12 + (m2 - m1);
}

// Return day-of-week (0=Sun) for a "YYYYMMDD" string.
int wdayOf(const char* ymd)
{
  struct tm t;
  memset(&t, 0, sizeof(t));
  int y, m, d;
  sscanf(ymd, "%4d%2d%2d", &y, &m, &d);
  t.tm_year = y - 1900;
  t.tm_mon  = m - 1;
  t.tm_mday = d;
  mktime(&t);
  return t.tm_wday;
}

// Check whether today appears in a comma-separated list of iCal datetimes.
// Handles both local and UTC (trailing Z) formats.
bool isExcludedToday(const char* exdates, const char* todayYMD)
{
  char buf[24];
  const char* p = exdates;
  while (*p) {
    while (*p == ',' || *p == ' ') p++;
    if (!*p) break;
    // Copy one value token
    int i = 0;
    while (*p && *p != ',' && i < (int)sizeof(buf) - 1) buf[i++] = *p++;
    buf[i] = '\0';
    // Parse through the same UTC-to-local logic
    ICalDT dt = parseICalDT(buf);
    char dtYMD[9];
    sprintf(dtYMD, "%04d%02d%02d", dt.year, dt.month, dt.day);
    if (strcmp(dtYMD, todayYMD) == 0) return true;
  }
  return false;
}

// Check if a recurring event (RRULE) fires on today's date.
// Now properly handles INTERVAL.
bool isRecurringToday(const char* startYMD, const char* rrule,
                      const char* todayYMD, int todayWday, int todayMday)
{
  if (strcmp(todayYMD, startYMD) < 0) return false;
  if (strcmp(todayYMD, startYMD) == 0) return true;  // start date always fires

  // Check UNTIL end date
  const char* u = strstr(rrule, "UNTIL=");
  if (u) {
    // UNTIL value might be "20260501" or "20260501T000000Z"
    char untilBuf[24];
    strncpy(untilBuf, u + 6, sizeof(untilBuf) - 1);
    untilBuf[sizeof(untilBuf) - 1] = '\0';
    // Terminate at ';' if present
    char* sc = strchr(untilBuf, ';');
    if (sc) *sc = '\0';
    ICalDT untilDt = parseICalDT(untilBuf);
    char untilYMD[9];
    sprintf(untilYMD, "%04d%02d%02d", untilDt.year, untilDt.month, untilDt.day);
    if (strcmp(todayYMD, untilYMD) > 0) return false;
  }

  // Parse INTERVAL (default 1)
  int interval = 1;
  const char* iv = strstr(rrule, "INTERVAL=");
  if (iv) interval = atoi(iv + 9);
  if (interval < 1) interval = 1;

  // --- DAILY ---
  if (strstr(rrule, "FREQ=DAILY")) {
    if (interval == 1) return true;
    return daysBetweenYMD(startYMD, todayYMD) % interval == 0;
  }

  // --- WEEKLY ---
  if (strstr(rrule, "FREQ=WEEKLY")) {
    // Check day-of-week
    bool dayMatch = false;
    const char* bd = strstr(rrule, "BYDAY=");
    if (bd) {
      static const char* abbr[] = {"SU","MO","TU","WE","TH","FR","SA"};
      const char* want = abbr[todayWday];
      const char* p = bd + 6;
      while (*p && *p != ';') {
        if (p[0] == want[0] && p[1] == want[1]) { dayMatch = true; break; }
        p++;
      }
    } else {
      dayMatch = (wdayOf(startYMD) == todayWday);
    }
    if (!dayMatch) return false;
    if (interval == 1) return true;
    int daysDiff = daysBetweenYMD(startYMD, todayYMD);
    int weeksDiff = daysDiff / 7;
    return weeksDiff % interval == 0;
  }

  // --- MONTHLY ---
  if (strstr(rrule, "FREQ=MONTHLY")) {
    int targetDay;
    const char* bm = strstr(rrule, "BYMONTHDAY=");
    if (bm) {
      targetDay = atoi(bm + 11);
    } else {
      targetDay = (startYMD[6] - '0') * 10 + (startYMD[7] - '0');
    }
    if (todayMday != targetDay) return false;
    if (interval == 1) return true;
    return monthsBetweenYMD(startYMD, todayYMD) % interval == 0;
  }

  // --- YEARLY ---
  if (strstr(rrule, "FREQ=YEARLY")) {
    if (strncmp(todayYMD + 4, startYMD + 4, 4) != 0) return false;
    if (interval == 1) return true;
    int y1, y2;
    sscanf(startYMD, "%4d", &y1);
    sscanf(todayYMD, "%4d", &y2);
    return (y2 - y1) % interval == 0;
  }

  return false;
}

// Try to add a parsed VEVENT to events[] if it occurs today.
void tryAddEvent(const char* summary, const char* dtstart, const char* dtend,
                 const char* duration, const char* rrule, const char* exdates,
                 const char* recurrenceId,
                 const char* todayYMD, int todayWday, int todayMday)
{
  if (!summary[0] || !dtstart[0]) return;
  if (eventCount >= MAX_EVENTS) return;

  // --- Handle RECURRENCE-ID (this is an override for one instance) ---
  bool isOverride = (recurrenceId[0] != '\0');
  if (isOverride) {
    ICalDT ridDt = parseICalDT(recurrenceId);
    char ridYMD[9];
    sprintf(ridYMD, "%04d%02d%02d", ridDt.year, ridDt.month, ridDt.day);
    if (strcmp(ridYMD, todayYMD) != 0) return;  // override for another day
    // Falls through to add using this event's DTSTART/DTEND
  }

  ICalDT sdt = parseICalDT(dtstart);
  char startYMD[9];
  sprintf(startYMD, "%04d%02d%02d", sdt.year, sdt.month, sdt.day);

  // --- Does this event occur today? ---
  bool today = false;
  if (recurrenceId[0]) {
    today = true;  // already confirmed above
  } else if (rrule[0]) {
    today = isRecurringToday(startYMD, rrule, todayYMD, todayWday, todayMday);
  } else {
    today = (strcmp(startYMD, todayYMD) == 0);
    // Multi-day event: check if today falls between start and end
    if (!today && dtend[0]) {
      ICalDT edt = parseICalDT(dtend);
      char endYMD[9];
      sprintf(endYMD, "%04d%02d%02d", edt.year, edt.month, edt.day);
      if (strcmp(startYMD, todayYMD) < 0 && strcmp(todayYMD, endYMD) < 0)
        today = true;
    }
  }
  if (!today) return;

  if (exdates[0] && isExcludedToday(exdates, todayYMD)) return;

  // --- If this is a RECURRENCE-ID override, replace any existing match ---
  if (isOverride) {
    for (int i = 0; i < eventCount; i++) {
      if (strcmp(events[i].title, summary) == 0) {
        // Replace the recurring instance with this override
        Serial.printf("  [OVERRIDE] \"%-.30s\" replacing existing entry\n", summary);
        // We'll fill events[i] below instead of events[eventCount]
        // Swap it to be rebuilt
        eventCount--;  // will be re-incremented at end
        if (i < eventCount)
          events[i] = events[eventCount]; // move last into this slot
        break;
      }
    }
  }

  // --- Build CalEvent ---
  CalEvent &ev = events[eventCount];
  strncpy(ev.title, summary, sizeof(ev.title) - 1);
  ev.title[sizeof(ev.title) - 1] = '\0';
  ev.allDay = sdt.dateOnly;

  if (!sdt.dateOnly) {
    ev.startHour = sdt.hour;
    ev.startMin  = sdt.min;

    if (dtend[0]) {
      ICalDT edt = parseICalDT(dtend);
      ev.endHour = edt.hour;
      ev.endMin  = edt.min;
    } else if (duration[0]) {
      int dH, dM;
      parseDuration(duration, dH, dM);
      ev.endMin  = ev.startMin + dM;
      ev.endHour = ev.startHour + dH + ev.endMin / 60;
      ev.endMin %= 60;
    } else {
      ev.endHour = ev.startHour + 1;
      ev.endMin  = ev.startMin;
    }
  } else {
    ev.startHour = HOUR_START; ev.startMin = 0;
    ev.endHour   = HOUR_END;   ev.endMin   = 0;
  }

  Serial.printf("  + \"%s\" %02d:%02d-%02d:%02d%s\n",
                ev.title, ev.startHour, ev.startMin,
                ev.endHour, ev.endMin,
                ev.allDay ? " [all-day]" : "");
  eventCount++;
}

// Sort events array by start time (insertion sort).
void sortEvents()
{
  for (int i = 1; i < eventCount; i++) {
    CalEvent key = events[i];
    int keyMin = key.startHour * 60 + key.startMin;
    int j = i - 1;
    while (j >= 0 && (events[j].startHour * 60 + events[j].startMin) > keyMin) {
      events[j + 1] = events[j];
      j--;
    }
    events[j + 1] = key;
  }
}

// Remove duplicate events (same title + same start time).
void dedup()
{
  for (int i = 0; i < eventCount; i++) {
    for (int j = i + 1; j < eventCount; ) {
      if (strcmp(events[i].title, events[j].title) == 0 &&
          events[i].startHour == events[j].startHour &&
          events[i].startMin  == events[j].startMin) {
        Serial.printf("  [DEDUP] removing duplicate \"%s\"\n", events[j].title);
        events[j] = events[eventCount - 1];
        eventCount--;
      } else {
        j++;
      }
    }
  }
}

// ============================================================
// Fetch calendar events via private iCal URL
// ============================================================

bool fetchCalendarEvents()
{
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected — skipping fetch.");
    return false;
  }

  struct tm nowTm;
  if (!getLocalTime(&nowTm)) {
    Serial.println("Can't get local time — skipping fetch.");
    return false;
  }
  char todayYMD[9];
  strftime(todayYMD, sizeof(todayYMD), "%Y%m%d", &nowTm);
  int todayWday = nowTm.tm_wday;
  int todayMday = nowTm.tm_mday;

  Serial.printf("Fetching iCal feed (today = %s)...\n", todayYMD);

  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();

  HTTPClient https;
  https.setTimeout(15000);
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setReuse(false);

  bool success = false;

  if (https.begin(*client, ICAL_URL)) {
    int httpCode = https.GET();
    Serial.printf("HTTP response: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      int contentLen = https.getSize();
      Serial.printf("Content-Length: %d (%d KB)\n",
                    contentLen, contentLen > 0 ? contentLen / 1024 : -1);
      WiFiClient *stream = https.getStreamPtr();
      eventCount = 0;

      bool inEvent = false;
      int  nested  = 0;

      static char summary[60], dtstart[24], dtend[24];
      static char dur[24], rrule[256], exdates[512], recurrenceId[24];
      summary[0] = dtstart[0] = dtend[0] = '\0';
      dur[0] = rrule[0] = exdates[0] = recurrenceId[0] = '\0';

      // Read line-by-line using a char buffer (avoids String overhead)
      static const int LINE_BUF_SZ = 512;
      static char lineBuf[LINE_BUF_SZ];
      int  lineLen = 0;

      // For iCal line unfolding, we accumulate into a "logical line"
      static const int LOGIC_BUF_SZ = 1024;
      static char logicBuf[LOGIC_BUF_SZ];
      int  logicLen = 0;

      unsigned long t0 = millis();
      unsigned long bytesRead = 0;
      int eventsScanned = 0;
      bool timedOut = false;

      auto processLogicLine = [&]() {
        if (logicLen == 0) return;
        logicBuf[logicLen] = '\0';
        const char* L = logicBuf;

        if (strcmp(L, "BEGIN:VEVENT") == 0) {
          inEvent = true;
          nested = 0;
          summary[0] = dtstart[0] = dtend[0] = '\0';
          dur[0] = rrule[0] = exdates[0] = recurrenceId[0] = '\0';
          eventsScanned++;
          if (eventsScanned % 1000 == 0)
            Serial.printf("  ... %d VEVENTs, %lu KB\n",
                          eventsScanned, bytesRead / 1024);
        }
        else if (inEvent && strncmp(L, "BEGIN:", 6) == 0) {
          nested++;
        }
        else if (inEvent && strncmp(L, "END:", 4) == 0) {
          if (nested > 0) {
            nested--;
          } else if (strcmp(L, "END:VEVENT") == 0) {
            inEvent = false;
            tryAddEvent(summary, dtstart, dtend, dur, rrule, exdates,
                        recurrenceId, todayYMD, todayWday, todayMday);
          }
        }
        else if (inEvent && nested == 0) {
          const char* colon = strchr(L, ':');
          if (colon) {
            const char* val = colon + 1;
            int keyLen = colon - L;
            const char* semi = (const char*)memchr(L, ';', keyLen);
            int propLen = semi ? (int)(semi - L) : keyLen;

            if      (propLen==7  && strncmp(L,"SUMMARY",7)==0)
              { strncpy(summary,val,sizeof(summary)-1); summary[sizeof(summary)-1]='\0'; }
            else if (propLen==7  && strncmp(L,"DTSTART",7)==0)
              { strncpy(dtstart,val,sizeof(dtstart)-1); dtstart[sizeof(dtstart)-1]='\0'; }
            else if (propLen==5  && strncmp(L,"DTEND",5)==0)
              { strncpy(dtend,val,sizeof(dtend)-1); dtend[sizeof(dtend)-1]='\0'; }
            else if (propLen==8  && strncmp(L,"DURATION",8)==0)
              { strncpy(dur,val,sizeof(dur)-1); dur[sizeof(dur)-1]='\0'; }
            else if (propLen==5  && strncmp(L,"RRULE",5)==0)
              { strncpy(rrule,val,sizeof(rrule)-1); rrule[sizeof(rrule)-1]='\0'; }
            else if (propLen==13 && strncmp(L,"RECURRENCE-ID",13)==0)
              { strncpy(recurrenceId,val,sizeof(recurrenceId)-1); recurrenceId[sizeof(recurrenceId)-1]='\0'; }
            else if (propLen==6  && strncmp(L,"EXDATE",6)==0) {
              if (exdates[0]) strncat(exdates,",",sizeof(exdates)-strlen(exdates)-1);
              strncat(exdates,val,sizeof(exdates)-strlen(exdates)-1);
            }
          }
        }
      };

      // --- Buffered chunk reading (much faster than byte-by-byte) ---
      static const int CHUNK_SZ = 4096;
      static uint8_t chunk[CHUNK_SZ];
      int chunkPos = 0, chunkLen = 0;

      while (true) {
        if (millis() - t0 > 120000) { timedOut = true; break; }  // 120s timeout

        // Refill chunk buffer when exhausted
        if (chunkPos >= chunkLen) {
          int avail = stream->available();
          if (avail > 0) {
            chunkLen = stream->readBytes(chunk, min(avail, CHUNK_SZ));
            chunkPos = 0;
          } else if (!stream->connected()) {
            break;  // server closed connection — we got all data
          } else {
            delay(2);
            continue;
          }
        }

        // Process bytes from the chunk
        while (chunkPos < chunkLen) {
          char b = (char)chunk[chunkPos++];
          bytesRead++;

          if (b == '\n') {
            if (lineLen > 0 && lineBuf[lineLen - 1] == '\r') lineLen--;
            lineBuf[lineLen] = '\0';

            if (lineLen > 0 && (lineBuf[0] == ' ' || lineBuf[0] == '\t')) {
              int appendLen = lineLen - 1;
              if (logicLen + appendLen < LOGIC_BUF_SZ - 1) {
                memcpy(logicBuf + logicLen, lineBuf + 1, appendLen);
                logicLen += appendLen;
              }
            } else {
              processLogicLine();
              logicLen = (lineLen < LOGIC_BUF_SZ - 1) ? lineLen : LOGIC_BUF_SZ - 1;
              memcpy(logicBuf, lineBuf, logicLen);
            }
            lineLen = 0;
          } else {
            if (lineLen < LINE_BUF_SZ - 1) lineBuf[lineLen++] = b;
          }
        }
      }
      // Process final buffered line
      processLogicLine();

      sortEvents();
      dedup();
      unsigned long elapsed = millis() - t0;
      Serial.printf("Loaded %d events for today (%lu KB, %d VEVENTs, %lus%s).\n",
                     eventCount, bytesRead / 1024,
                     eventsScanned, elapsed / 1000,
                     timedOut ? " TIMEOUT" : "");
      success = true;
    } else {
      Serial.printf("HTTP GET failed: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();
  } else {
    Serial.println("HTTPS connection failed.");
  }

  delete client;
  return success;
}

// ============================================================
// Layout helpers
// ============================================================

// Convert a time (hour, minute) to a Y pixel on the timeline.
// Clamps to timeline bounds.
int timeToY(int hour, int minute)
{
  int mins = (hour - HOUR_START) * 60 + minute;
  if (mins < 0) mins = 0;
  if (mins > TOTAL_MINUTES) mins = TOTAL_MINUTES;
  return TIMELINE_Y + (int)((long)mins * TIMELINE_H / TOTAL_MINUTES);
}

// Truncate a string to fit within maxWidth pixels, adding "…" if needed.
// Uses the currently set font. Returns the truncated string in outBuf.
void truncateToFit(const char* text, int maxWidth, char* outBuf, int outBufSize)
{
  int16_t tx, ty;
  uint16_t tw, th;

  // Check if full string fits
  display.getTextBounds(text, 0, 0, &tx, &ty, &tw, &th);
  if ((int)tw <= maxWidth) {
    strncpy(outBuf, text, outBufSize - 1);
    outBuf[outBufSize - 1] = '\0';
    return;
  }

  // Binary search for the max chars that fit with "…"
  int len = strlen(text);
  int lo = 0, hi = len;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    char tmp[64];
    int cpLen = (mid < (int)sizeof(tmp) - 2) ? mid : (int)sizeof(tmp) - 2;
    strncpy(tmp, text, cpLen);
    tmp[cpLen] = '\0';
    // Append ellipsis character
    strcat(tmp, "~");  // Use ~ as stand-in for measuring (similar width to …)
    display.getTextBounds(tmp, 0, 0, &tx, &ty, &tw, &th);
    if ((int)tw <= maxWidth) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }

  int cpLen = lo;
  if (cpLen > outBufSize - 4) cpLen = outBufSize - 4;
  strncpy(outBuf, text, cpLen);
  outBuf[cpLen] = '\0';
  // UTF-8 ellipsis: "…" = 0xE2 0x80 0xA6
  outBuf[cpLen]     = (char)0xE2;
  outBuf[cpLen + 1] = (char)0x80;
  outBuf[cpLen + 2] = (char)0xA6;
  outBuf[cpLen + 3] = '\0';
}

// ============================================================
// Drawing functions
// ============================================================

void drawHeader()
{
  struct tm t;
  if (!getLocalTime(&t)) return;

  // e.g., "Monday, April 14"
  char dateBuf[40];
  strftime(dateBuf, sizeof(dateBuf), "%A, %B %d", &t);

  display.setFont(&FreeSansBold18pt7b);
  display.setTextColor(GxEPD_BLACK);

  int16_t tx, ty;
  uint16_t tw, th;
  display.getTextBounds(dateBuf, 0, 0, &tx, &ty, &tw, &th);

  // Center horizontally, vertically within header
  int x = (SCREEN_W - (int)tw) / 2;
  int y = (HEADER_H + (int)th) / 2;  // baseline position
  display.setCursor(x, y);
  display.print(dateBuf);

  // Separator line below header
  display.drawFastHLine(0, HEADER_H - 1, SCREEN_W, GxEPD_BLACK);
}

void drawHourGrid()
{
  display.setFont(&FreeSans9pt7b);
  display.setTextColor(GxEPD_BLACK);

  for (int h = HOUR_START; h <= HOUR_END; h++) {
    int y = timeToY(h, 0);

    // Hour gridline (light dashed — draw dotted)
    for (int x = LABEL_W; x < SCREEN_W; x += 4) {
      display.drawPixel(x, y, GxEPD_BLACK);
    }

    // Hour label: "9AM", "10", "11", "12PM", "1", ...
    if (h == HOUR_END) break;  // Don't label the bottom boundary

    char label[6];
    if (h == 12) {
      strcpy(label, "12PM");
    } else if (h < 12) {
      sprintf(label, "%d AM", h);
    } else {
      sprintf(label, "%d PM", h - 12);
    }

    int16_t tx, ty;
    uint16_t tw, th;
    display.getTextBounds(label, 0, 0, &tx, &ty, &tw, &th);

    // Right-align label in the gutter, vertically centered on the gridline
    int lx = LABEL_W - (int)tw - 3;
    int ly = y + (int)th / 2;
    display.setCursor(lx, ly);
    display.print(label);
  }

  // Vertical separator between labels and events
  display.drawFastVLine(LABEL_W, TIMELINE_Y, TIMELINE_H, GxEPD_BLACK);
}

void drawEvents()
{
  for (int i = 0; i < eventCount; i++) {
    CalEvent &ev = events[i];

    // Skip all-day events (rendered separately) or events fully outside 9-5
    if (ev.allDay) continue;
    if (ev.endHour < HOUR_START || (ev.endHour == HOUR_START && ev.endMin == 0)) continue;
    if (ev.startHour >= HOUR_END) continue;

    // Clamp to timeline bounds
    int sh = ev.startHour, sm = ev.startMin;
    int eh = ev.endHour,   em = ev.endMin;
    if (sh < HOUR_START || (sh == HOUR_START && sm < 0)) { sh = HOUR_START; sm = 0; }
    if (eh > HOUR_END || (eh == HOUR_END && em > 0))     { eh = HOUR_END;   em = 0; }

    int y1 = timeToY(sh, sm);
    int y2 = timeToY(eh, em);
    int blockH = y2 - y1;
    if (blockH < 2) blockH = 2;

    // Draw filled block
    display.fillRect(EVENT_X, y1, EVENT_W, blockH, GxEPD_BLACK);

    // Draw title (inverted: white on black)
    if (blockH >= 16) {
      display.setFont(&FreeSansBold9pt7b);
      display.setTextColor(GxEPD_WHITE);

      char truncated[64];
      truncateToFit(ev.title, EVENT_W - 8, truncated, sizeof(truncated));

      int16_t tx, ty;
      uint16_t tw, th;
      display.getTextBounds(truncated, 0, 0, &tx, &ty, &tw, &th);

      int textY = y1 + 3 + (int)th;  // 3px top padding + ascent
      display.setCursor(EVENT_X + 4, textY);
      display.print(truncated);

      // If block is tall enough, show time range on second line
      if (blockH >= 34) {
        char timeBuf[20];
        sprintf(timeBuf, "%d:%02d - %d:%02d",
                ev.startHour > 12 ? ev.startHour - 12 : ev.startHour, ev.startMin,
                ev.endHour > 12 ? ev.endHour - 12 : ev.endHour, ev.endMin);

        display.setFont(&FreeSans9pt7b);
        display.setCursor(EVENT_X + 4, textY + (int)th + 3);
        display.print(timeBuf);
      }

      display.setTextColor(GxEPD_BLACK);
    }
  }

  // All-day events: banner between header and timeline start
  bool hasAllDay = false;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].allDay) { hasAllDay = true; break; }
  }

  if (hasAllDay) {
    int bannerY = TIMELINE_Y;
    display.setFont(&FreeSans9pt7b);
    display.setTextColor(GxEPD_BLACK);
    for (int i = 0; i < eventCount; i++) {
      if (!events[i].allDay) continue;

      int16_t tx, ty;
      uint16_t tw, th;
      display.getTextBounds(events[i].title, 0, 0, &tx, &ty, &tw, &th);

      // Draw a small outlined banner
      display.drawRect(EVENT_X, bannerY + 2, EVENT_W, (int)th + 6, GxEPD_BLACK);
      display.setCursor(EVENT_X + 4, bannerY + 2 + (int)th + 2);
      display.print(events[i].title);
      bannerY += (int)th + 10;
    }
  }
}

void drawNowIndicator()
{
  struct tm t;
  if (!getLocalTime(&t)) return;

  int hour = t.tm_hour;
  int minute = t.tm_min;

  // Only draw if within the timeline range
  if (hour < HOUR_START || hour >= HOUR_END) return;

  int y = timeToY(hour, minute);
  prevNowLineY = y;

  // Draw a solid line with a small triangle marker on the left edge
  display.drawFastHLine(LABEL_W + 1, y, SCREEN_W - LABEL_W - 1, GxEPD_BLACK);
  // Small filled triangle on the left as the "now" indicator
  display.fillTriangle(
    LABEL_W + 1, y,
    LABEL_W + 7, y - 4,
    LABEL_W + 7, y + 4,
    GxEPD_BLACK
  );
}

void drawFooter()
{
  struct tm t;
  if (!getLocalTime(&t)) return;

  // Separator line
  display.drawFastHLine(0, FOOTER_Y, SCREEN_W, GxEPD_BLACK);

  char buf[30];
  strftime(buf, sizeof(buf), "Updated %H:%M", &t);

  display.setFont();  // Built-in 6×8 font
  display.setTextSize(1);
  display.setTextColor(GxEPD_BLACK);
  display.setCursor(4, FOOTER_Y + 5);
  display.print(buf);

  // Show event count on the right
  char evBuf[16];
  sprintf(evBuf, "%d events", eventCount);
  int evW = strlen(evBuf) * 6;
  display.setCursor(SCREEN_W - evW - 4, FOOTER_Y + 5);
  display.print(evBuf);
}

// ============================================================
// Full screen draw (paged for memory efficiency)
// ============================================================

void drawFullScreen()
{
  Serial.println("Drawing full screen...");
  display.setRotation(DISPLAY_ROTATION);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader();
    drawHourGrid();
    drawEvents();
    drawNowIndicator();
    drawFooter();
  } while (display.nextPage());
  Serial.println("Full screen draw complete.");
}

// ============================================================
// Partial refresh: just the "now" line area
// ============================================================

void updateNowLine()
{
  struct tm t;
  if (!getLocalTime(&t)) return;

  int hour = t.tm_hour;
  int minute = t.tm_min;

  // If outside timeline range, nothing to draw
  if (hour < HOUR_START || hour >= HOUR_END) {
    // If we had a previous line, we'd need a full redraw to clear it
    if (prevNowLineY >= 0) {
      drawFullScreen();
      prevNowLineY = -1;
    }
    return;
  }

  int newY = timeToY(hour, minute);

  // If the line hasn't moved a pixel, skip
  if (newY == prevNowLineY) return;

  // Redraw the full screen via partial refresh (avoids flash)
  // This is cleaner than trying to erase/redraw just the line region
  // since events might overlap the old line position
  Serial.printf("Now line: %02d:%02d → y=%d (was %d)\n", hour, minute, newY, prevNowLineY);

  display.setRotation(DISPLAY_ROTATION);
  display.setPartialWindow(0, 0, SCREEN_W, SCREEN_H);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader();
    drawHourGrid();
    drawEvents();
    drawNowIndicator();
    drawFooter();
  } while (display.nextPage());

  Serial.println("Partial refresh complete.");
}

// ============================================================
// Setup
// ============================================================

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n========================================");
  Serial.println("  E-Ink Calendar Dashboard — Starting");
  Serial.println("========================================");

  // Power on the e-paper display
  pinMode(EPD_POWER, OUTPUT);
  digitalWrite(EPD_POWER, HIGH);
  delay(500);
  Serial.println("[HW] Display power ON (GPIO 7)");

  // Initialize custom SPI bus
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(DISPLAY_ROTATION);
  Serial.printf("[HW] Display init OK — %dx%d (rotation %d)\n",
                display.width(), display.height(), DISPLAY_ROTATION);

  // Connect WiFi
  connectWiFi();

  // Sync time via NTP
  syncTime();

  // Fetch calendar events
  fetchCalendarEvents();
  lastFetch = millis();

  // Initial full-refresh draw
  drawFullScreen();
  lastFullRefresh = millis();
  lastNowLineUpdate = millis();

  Serial.println("[OK] Dashboard is running.");
  Serial.println("========================================\n");
}

// ============================================================
// Main loop
// ============================================================

void loop()
{
  unsigned long now = millis();

  // --- Re-fetch calendar data every FETCH_INTERVAL ---
  if (now - lastFetch >= FETCH_INTERVAL) {
    // Reconnect WiFi if dropped
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
    fetchCalendarEvents();
    lastFetch = now;

    // After new data, do a full partial redraw to update events
    updateNowLine();
    lastNowLineUpdate = now;
  }

  // --- Full refresh every FULL_REFRESH_INTERVAL (clears ghosting) ---
  if (now - lastFullRefresh >= FULL_REFRESH_INTERVAL) {
    // Re-sync time once per hour too
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
    syncTime();
    drawFullScreen();
    lastFullRefresh = now;
    lastNowLineUpdate = now;
  }

  // --- Update "now" line every NOW_LINE_INTERVAL ---
  if (now - lastNowLineUpdate >= NOW_LINE_INTERVAL) {
    updateNowLine();
    lastNowLineUpdate = now;
  }

  delay(1000);  // Sleep 1s between checks
}
