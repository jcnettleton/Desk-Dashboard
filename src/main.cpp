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
#include <ArduinoJson.h>

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
static const int FOOTER_Y          = SCREEN_H - FOOTER_H;

static const int HOUR_START        = 9;      // 9 AM
static const int HOUR_END          = 17;     // 5 PM
static const int TOTAL_MINUTES     = (HOUR_END - HOUR_START) * 60;  // 480

static const int LABEL_W           = 30;     // Width of hour-label gutter
static const int EVENT_X           = LABEL_W + 2;
static const int EVENT_W           = SCREEN_W - EVENT_X - 4;
static const int EVENT_GAP         = 2;      // px gap between adjacent event blocks
static const int EVENT_RADIUS      = 4;      // rounded corner radius

static const int NOW_BALL_R        = 10;     // radius of the "now" ball indicator

// Dynamic timeline bounds — adjusted at runtime when all-day banners are present
static int TIMELINE_Y = HEADER_H;
static int TIMELINE_H = SCREEN_H - HEADER_H - FOOTER_H; // 732px default

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
static const unsigned long FETCH_INTERVAL      = 60UL * 1000;       // 60 sec
static const unsigned long FULL_REFRESH_INTERVAL = 60UL * 60 * 1000; // 1 hour

unsigned long lastNowLineUpdate  = 0;
unsigned long lastFetch          = 0;
unsigned long lastFullRefresh    = 0;
int           prevNowLineY       = -1;

// Forward declarations
void drawFullScreen();
void drawPartialScreen();

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
// Sort events array by start time (insertion sort)
// ============================================================

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

// ============================================================
// Fetch calendar events via Google Apps Script (JSON)
// ============================================================

bool fetchCalendarEvents()
{
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected — skipping fetch.");
    return false;
  }

  Serial.println("Fetching calendar via Apps Script...");

  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();

  HTTPClient https;
  https.setTimeout(15000);
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.setReuse(false);

  bool success = false;

  if (https.begin(*client, APPS_SCRIPT_URL)) {
    int httpCode = https.GET();
    Serial.printf("HTTP response: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String payload = https.getString();
      Serial.printf("Payload: %d bytes\n", payload.length());

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, payload);
      if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
      } else if (!doc.is<JsonArray>()) {
        // Could be an error object like {"error":"unauthorized"}
        const char* error = doc["error"];
        Serial.printf("API error: %s\n", error ? error : "unexpected response");
      } else {
        JsonArray arr = doc.as<JsonArray>();
        eventCount = 0;

        for (JsonObject ev : arr) {
          if (eventCount >= MAX_EVENTS) break;

          const char* title = ev["title"] | "";
          const char* start = ev["start"] | "";
          const char* end   = ev["end"]   | "";
          bool allDay       = ev["allDay"] | false;

          CalEvent &ce = events[eventCount];
          strncpy(ce.title, title, sizeof(ce.title) - 1);
          ce.title[sizeof(ce.title) - 1] = '\0';
          ce.allDay = allDay;

          if (allDay) {
            ce.startHour = HOUR_START; ce.startMin = 0;
            ce.endHour   = HOUR_END;   ce.endMin   = 0;
          } else {
            // Parse ISO 8601 timestamps (e.g. "2026-04-15T09:00:00.000Z")
            // Convert UTC to local time
            struct tm utcTm;
            memset(&utcTm, 0, sizeof(utcTm));
            sscanf(start, "%4d-%2d-%2dT%2d:%2d",
                   &utcTm.tm_year, &utcTm.tm_mon, &utcTm.tm_mday,
                   &utcTm.tm_hour, &utcTm.tm_min);
            utcTm.tm_year -= 1900;
            utcTm.tm_mon  -= 1;

            setenv("TZ", "UTC0", 1); tzset();
            time_t epoch = mktime(&utcTm);
            setenv("TZ", TIMEZONE, 1); tzset();
            struct tm loc;
            localtime_r(&epoch, &loc);
            ce.startHour = loc.tm_hour;
            ce.startMin  = loc.tm_min;

            memset(&utcTm, 0, sizeof(utcTm));
            sscanf(end, "%4d-%2d-%2dT%2d:%2d",
                   &utcTm.tm_year, &utcTm.tm_mon, &utcTm.tm_mday,
                   &utcTm.tm_hour, &utcTm.tm_min);
            utcTm.tm_year -= 1900;
            utcTm.tm_mon  -= 1;

            setenv("TZ", "UTC0", 1); tzset();
            epoch = mktime(&utcTm);
            setenv("TZ", TIMEZONE, 1); tzset();
            localtime_r(&epoch, &loc);
            ce.endHour = loc.tm_hour;
            ce.endMin  = loc.tm_min;
          }

          Serial.printf("  + \"%s\" %02d:%02d-%02d:%02d%s\n",
                        ce.title, ce.startHour, ce.startMin,
                        ce.endHour, ce.endMin,
                        ce.allDay ? " [all-day]" : "");
          eventCount++;
        }

        sortEvents();
        Serial.printf("Loaded %d events for today.\n", eventCount);
        success = true;
      }
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

// Compute a simple hash of the current event data for change detection
static uint32_t computeEventsHash()
{
  uint32_t h = 5381;
  h = h * 33 + (uint32_t)eventCount;
  for (int i = 0; i < eventCount; i++) {
    const CalEvent &e = events[i];
    for (const char* p = e.title; *p; p++) h = h * 33 + (uint8_t)*p;
    h = h * 33 + (uint32_t)e.startHour;
    h = h * 33 + (uint32_t)e.startMin;
    h = h * 33 + (uint32_t)e.endHour;
    h = h * 33 + (uint32_t)e.endMin;
    h = h * 33 + (uint32_t)e.allDay;
  }
  return h;
}

static uint32_t lastEventsHash = 0;

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

  display.setFont(&FreeSansBold12pt7b);
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
  display.setFont();        // Built-in 6×8 font for compact labels
  display.setTextSize(1);
  display.setTextColor(GxEPD_BLACK);

  for (int h = HOUR_START; h <= HOUR_END; h++) {
    int y = timeToY(h, 0);

    // Hour gridline (light dashed — draw dotted)
    for (int x = LABEL_W; x < SCREEN_W; x += 4) {
      display.drawPixel(x, y, GxEPD_BLACK);
    }

    if (h == HOUR_END) break;    // Don't label the bottom boundary
    if (h == HOUR_START) continue; // Skip 9am — obvious from context

    char label[6];
    if (h == 12) {
      strcpy(label, "12pm");
    } else if (h < 12) {
      sprintf(label, "%dam", h);
    } else {
      sprintf(label, "%dpm", h - 12);
    }

    // Built-in font: 6px wide per char, 8px tall
    int tw = strlen(label) * 6;
    int lx = LABEL_W - tw - 1;
    int ly = y - 4;  // vertically center the 8px-tall text on the gridline
    display.setCursor(lx, ly);
    display.print(label);
  }

  // Vertical separator between labels and events
  display.drawFastVLine(LABEL_W, TIMELINE_Y, TIMELINE_H, GxEPD_BLACK);
}

// Compute overlap columns for timed events.
// Each timed event gets a column index and a column count.
static int evCol[MAX_EVENTS];      // column index (0-based)
static int evColCount[MAX_EVENTS]; // total columns in this overlap group

void computeOverlapColumns()
{
  // Collect indices of visible timed events (already sorted by start time)
  int vis[MAX_EVENTS];
  int visN = 0;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].allDay) continue;
    int sh = events[i].startHour, sm = events[i].startMin;
    int eh = events[i].endHour,   em = events[i].endMin;
    int startMin = sh * 60 + sm;
    int endMin   = eh * 60 + em;
    if (endMin <= HOUR_START * 60 || startMin >= HOUR_END * 60) continue;
    vis[visN++] = i;
  }

  // Initialize all to column 0, count 1
  for (int i = 0; i < eventCount; i++) { evCol[i] = 0; evColCount[i] = 1; }

  // For each visible event, find overlapping events and assign columns
  for (int a = 0; a < visN; a++) {
    int ai = vis[a];
    int aStart = events[ai].startHour * 60 + events[ai].startMin;
    int aEnd   = events[ai].endHour   * 60 + events[ai].endMin;

    // Find all events that overlap with this one
    bool colUsed[MAX_EVENTS] = {};
    for (int b = 0; b < a; b++) {
      int bi = vis[b];
      int bStart = events[bi].startHour * 60 + events[bi].startMin;
      int bEnd   = events[bi].endHour   * 60 + events[bi].endMin;
      if (bStart < aEnd && bEnd > aStart) {
        colUsed[evCol[bi]] = true;
      }
    }
    // Assign first free column
    int col = 0;
    while (colUsed[col]) col++;
    evCol[ai] = col;
  }

  // Compute column count per overlap group
  for (int a = 0; a < visN; a++) {
    int ai = vis[a];
    int aStart = events[ai].startHour * 60 + events[ai].startMin;
    int aEnd   = events[ai].endHour   * 60 + events[ai].endMin;
    int maxCol = evCol[ai];
    for (int b = 0; b < visN; b++) {
      int bi = vis[b];
      int bStart = events[bi].startHour * 60 + events[bi].startMin;
      int bEnd   = events[bi].endHour   * 60 + events[bi].endMin;
      if (bStart < aEnd && bEnd > aStart) {
        if (evCol[bi] > maxCol) maxCol = evCol[bi];
      }
    }
    evColCount[ai] = maxCol + 1;
  }
}

void drawEvents()
{
  computeOverlapColumns();

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

    // Compute column-based X and width
    int cols = evColCount[i];
    int col  = evCol[i];
    int colW = EVENT_W / cols;
    int ex   = EVENT_X + col * colW;
    int ew   = (col == cols - 1) ? (EVENT_W - col * colW) : colW; // last col gets remainder

    // Inset for gaps between adjacent blocks
    int drawX = ex + EVENT_GAP;
    int drawY = y1 + EVENT_GAP;
    int drawW = ew - EVENT_GAP * 2;
    int drawH = blockH - EVENT_GAP;
    if (drawW < 4) drawW = 4;
    if (drawH < 2) drawH = 2;

    display.fillRoundRect(drawX, drawY, drawW, drawH, EVENT_RADIUS, GxEPD_BLACK);

    // Draw title (inverted: white on black)
    if (drawH >= 14) {
      display.setFont(&FreeSans9pt7b);
      display.setTextColor(GxEPD_WHITE);

      char truncated[64];
      truncateToFit(ev.title, drawW - 8, truncated, sizeof(truncated));

      int16_t tx, ty;
      uint16_t tw, th;
      display.getTextBounds(truncated, 0, 0, &tx, &ty, &tw, &th);

      int textY = drawY + 3 + (int)th;  // 3px top padding + ascent
      display.setCursor(drawX + 4, textY);
      display.print(truncated);

      // If block is tall enough, show time range on second line
      if (drawH >= 30) {
        char timeBuf[20];
        sprintf(timeBuf, "%d:%02d - %d:%02d",
                ev.startHour > 12 ? ev.startHour - 12 : ev.startHour, ev.startMin,
                ev.endHour > 12 ? ev.endHour - 12 : ev.endHour, ev.endMin);

        display.setFont();
        display.setTextSize(1);
        display.setCursor(drawX + 4, textY + 4);
        display.print(timeBuf);
      }

      display.setTextColor(GxEPD_BLACK);
    }
  }
}

void drawAllDayBanners()
{
  bool hasAllDay = false;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].allDay) { hasAllDay = true; break; }
  }
  if (!hasAllDay) return;

  int bannerY = HEADER_H;
  display.setFont(&FreeSans9pt7b);
  display.setTextColor(GxEPD_BLACK);

  for (int i = 0; i < eventCount; i++) {
    if (!events[i].allDay) continue;

    int16_t tx, ty;
    uint16_t tw, th;
    display.getTextBounds(events[i].title, 0, 0, &tx, &ty, &tw, &th);

    int bannerH = (int)th + 6;
    display.drawRoundRect(EVENT_X, bannerY + 2, EVENT_W, bannerH, 3, GxEPD_BLACK);
    display.setCursor(EVENT_X + 4, bannerY + 2 + (int)th + 2);
    display.print(events[i].title);
    bannerY += bannerH + 4;
  }

  // Shift timeline down below the banners
  TIMELINE_Y = bannerY + 2;
  TIMELINE_H = FOOTER_Y - TIMELINE_Y;
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

  // Draw filled black ball centered in the gutter
  int cx = LABEL_W / 2;
  display.fillCircle(cx, y, NOW_BALL_R, GxEPD_BLACK);

  // Redraw any hour labels that overlap the ball in white (inverted)
  display.setFont();        // Built-in 6×8 font
  display.setTextSize(1);

  for (int h = HOUR_START + 1; h < HOUR_END; h++) {
    int hy = timeToY(h, 0);
    // Label is drawn at ly = hy-4, height 8px, so it spans [hy-4, hy+4)
    int labelTop = hy - 4;
    int labelBot = hy + 4;
    int ballTop  = y - NOW_BALL_R;
    int ballBot  = y + NOW_BALL_R;

    if (labelBot <= ballTop || labelTop >= ballBot) continue;

    char label[6];
    if (h == 12) {
      strcpy(label, "12pm");
    } else if (h < 12) {
      sprintf(label, "%dam", h);
    } else {
      sprintf(label, "%dpm", h - 12);
    }

    int tw = strlen(label) * 6;
    int lx = LABEL_W - tw - 1;
    int ly = hy - 4;
    display.setTextColor(GxEPD_WHITE);
    display.setCursor(lx, ly);
    display.print(label);
  }

  display.setTextColor(GxEPD_BLACK);
}

void drawFooter()
{
  struct tm t;
  if (!getLocalTime(&t)) return;

  // Separator line
  display.drawFastHLine(0, FOOTER_Y, SCREEN_W, GxEPD_BLACK);

  char buf[30];
  strftime(buf, sizeof(buf), "Updated %l:%M %p", &t);

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
  display.init(0, false);  // wake display, initial=false to avoid double-refresh
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);  // re-assert custom SPI pins
  display.setRotation(DISPLAY_ROTATION);
  display.setFullWindow();
  // Reset timeline bounds to defaults
  TIMELINE_Y = HEADER_H;
  TIMELINE_H = SCREEN_H - HEADER_H - FOOTER_H;

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader();
    drawAllDayBanners();
    drawHourGrid();
    drawEvents();
    drawNowIndicator();
    drawFooter();
  } while (display.nextPage());
  display.hibernate();
  Serial.println("Full screen draw complete.");
}

// ============================================================
// Partial refresh: full redraw without flash
// ============================================================

void drawPartialScreen()
{
  Serial.println("Drawing partial screen...");
  display.init(0, false);  // wake display, initial=false to allow partial refresh
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);  // re-assert custom SPI pins
  display.setRotation(DISPLAY_ROTATION);
  // Reset timeline bounds to defaults
  TIMELINE_Y = HEADER_H;
  TIMELINE_H = SCREEN_H - HEADER_H - FOOTER_H;

  display.setPartialWindow(0, 0, SCREEN_W, SCREEN_H);
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    drawHeader();
    drawAllDayBanners();
    drawHourGrid();
    drawEvents();
    drawNowIndicator();
    drawFooter();
  } while (display.nextPage());
  display.hibernate();
  Serial.println("Partial refresh complete.");
}

// ============================================================
// Update now ball position (calls partial refresh)
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
  drawPartialScreen();
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
  lastEventsHash = computeEventsHash();

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

    // Only redraw if event data actually changed
    uint32_t newHash = computeEventsHash();
    if (newHash != lastEventsHash) {
      Serial.println("Calendar data changed — refreshing display.");
      lastEventsHash = newHash;
      drawPartialScreen();
      lastNowLineUpdate = now;
    }
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
