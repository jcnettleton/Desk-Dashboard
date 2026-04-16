// ============================================================
// E-Ink Calendar Simulator — pixel-perfect native C++ build
// ============================================================
// Uses Adafruit GFX's GFXcanvas1 (1-bit in-memory framebuffer)
// with the SAME font data and drawing code as the ESP32 firmware.
// Output: BMP file identical to what the e-paper displays.
//
// Build:  make -C simulator
// Usage:  ./simulator/sim              (sample events, current time)
//         ./simulator/sim --time 10:30
//         ./simulator/sim --events events.json
// ============================================================

#include "Arduino.h"
#include "Adafruit_GFX.h"

// The exact same fonts used by the firmware
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// ---- GxEPD2 color aliases ----
#define GxEPD_BLACK 0
#define GxEPD_WHITE 1

// ---- Layout constants (must match main.cpp exactly) ----
static const int SCREEN_W      = 272;
static const int SCREEN_H      = 792;
static const int HEADER_H      = 42;
static const int FOOTER_H      = 18;
static const int FOOTER_Y      = SCREEN_H - FOOTER_H;
static const int HOUR_START    = 9;
static const int HOUR_END      = 17;
static const int TOTAL_MINUTES = (HOUR_END - HOUR_START) * 60;    // 480
static const int LABEL_W       = 30;
static const int EVENT_X       = LABEL_W + 2;
static const int EVENT_W       = SCREEN_W - EVENT_X - 4;
static const int EVENT_GAP     = 2;   // px gap between adjacent event blocks
static const int EVENT_RADIUS  = 4;   // rounded corner radius
static const int NOW_BALL_R    = 10;  // radius of the "now" ball indicator

// Dynamic timeline bounds — adjusted at runtime when all-day banners are present
static int TIMELINE_Y = HEADER_H;
static int TIMELINE_H = SCREEN_H - HEADER_H - FOOTER_H;  // 732 default

// ---- Event storage (same struct as firmware) ----
struct CalEvent {
  char title[60];
  int  startHour, startMin;
  int  endHour,   endMin;
  bool allDay;
};
static const int MAX_EVENTS = 20;
static CalEvent events[MAX_EVENTS];
static int eventCount = 0;

// ---- The canvas (replaces GxEPD2 display object) ----
static GFXcanvas1 canvas(SCREEN_W, SCREEN_H);

// ---- Simulated time ----
static struct tm simTime;

// ============================================================
// Layout helpers (identical to main.cpp)
// ============================================================

int timeToY(int hour, int minute)
{
  int mins = (hour - HOUR_START) * 60 + minute;
  if (mins < 0) mins = 0;
  if (mins > TOTAL_MINUTES) mins = TOTAL_MINUTES;
  return TIMELINE_Y + (int)((long)mins * TIMELINE_H / TOTAL_MINUTES);
}

void truncateToFit(const char* text, int maxWidth, char* outBuf, int outBufSize)
{
  int16_t tx, ty;
  uint16_t tw, th;

  canvas.getTextBounds(text, 0, 0, &tx, &ty, &tw, &th);
  if ((int)tw <= maxWidth) {
    strncpy(outBuf, text, outBufSize - 1);
    outBuf[outBufSize - 1] = '\0';
    return;
  }

  int len = strlen(text);
  int lo = 0, hi = len;
  while (lo < hi) {
    int mid = (lo + hi + 1) / 2;
    char tmp[64];
    int cpLen = (mid < (int)sizeof(tmp) - 2) ? mid : (int)sizeof(tmp) - 2;
    strncpy(tmp, text, cpLen);
    tmp[cpLen] = '\0';
    strcat(tmp, "~");
    canvas.getTextBounds(tmp, 0, 0, &tx, &ty, &tw, &th);
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
  // UTF-8 ellipsis
  outBuf[cpLen]     = (char)0xE2;
  outBuf[cpLen + 1] = (char)0x80;
  outBuf[cpLen + 2] = (char)0xA6;
  outBuf[cpLen + 3] = '\0';
}

// ============================================================
// Drawing functions (identical logic to main.cpp, using canvas)
// ============================================================

void drawHeader()
{
  char dateBuf[40];
  strftime(dateBuf, sizeof(dateBuf), "%A, %B %d", &simTime);

  canvas.setFont(&FreeSansBold12pt7b);
  canvas.setTextColor(GxEPD_BLACK);

  int16_t tx, ty;
  uint16_t tw, th;
  canvas.getTextBounds(dateBuf, 0, 0, &tx, &ty, &tw, &th);

  int x = (SCREEN_W - (int)tw) / 2;
  int y = (HEADER_H + (int)th) / 2;
  canvas.setCursor(x, y);
  canvas.print(dateBuf);

  canvas.drawFastHLine(0, HEADER_H - 1, SCREEN_W, GxEPD_BLACK);
}

void drawHourGrid()
{
  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setTextColor(GxEPD_BLACK);

  for (int h = HOUR_START; h <= HOUR_END; h++) {
    int y = timeToY(h, 0);

    for (int x = LABEL_W; x < SCREEN_W; x += 4) {
      canvas.drawPixel(x, y, GxEPD_BLACK);
    }

    if (h == HOUR_END) break;
    if (h == HOUR_START) continue;

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
    int ly = y - 4;
    canvas.setCursor(lx, ly);
    canvas.print(label);
  }

  canvas.drawFastVLine(LABEL_W, TIMELINE_Y, TIMELINE_H, GxEPD_BLACK);
}

// Overlap column computation (identical to main.cpp)
static int evCol[MAX_EVENTS];
static int evColCount[MAX_EVENTS];

void computeOverlapColumns()
{
  int vis[MAX_EVENTS];
  int visN = 0;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].allDay) continue;
    int startMin = events[i].startHour * 60 + events[i].startMin;
    int endMin   = events[i].endHour * 60 + events[i].endMin;
    if (endMin <= HOUR_START * 60 || startMin >= HOUR_END * 60) continue;
    vis[visN++] = i;
  }

  for (int i = 0; i < eventCount; i++) { evCol[i] = 0; evColCount[i] = 1; }

  for (int a = 0; a < visN; a++) {
    int ai = vis[a];
    int aStart = events[ai].startHour * 60 + events[ai].startMin;
    int aEnd   = events[ai].endHour   * 60 + events[ai].endMin;

    bool colUsed[MAX_EVENTS] = {};
    for (int b = 0; b < a; b++) {
      int bi = vis[b];
      int bStart = events[bi].startHour * 60 + events[bi].startMin;
      int bEnd   = events[bi].endHour   * 60 + events[bi].endMin;
      if (bStart < aEnd && bEnd > aStart) {
        colUsed[evCol[bi]] = true;
      }
    }
    int col = 0;
    while (colUsed[col]) col++;
    evCol[ai] = col;
  }

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

// Fill a rounded rect with Atkinson-dithered diagonal gradient
void fillRoundRectDithered(GFXcanvas1 &c, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r)
{
  c.fillRoundRect(x, y, w, h, r, GxEPD_BLACK);

  if (w < 3 || h < 3) return;

  int iw = w - 2, ih = h - 2;
  int n = iw * ih;
  int16_t *buf = (int16_t *)malloc(n * sizeof(int16_t));
  if (!buf) return;

  float lo = 35.0f;
  float hi = 215.0f;
  float range = hi - lo;
  float denom = (iw + ih > 2) ? (float)(iw + ih - 2) : 1.0f;
  for (int row = 0; row < ih; row++) {
    for (int col = 0; col < iw; col++) {
      float t = (float)(col + row) / denom;
      buf[row * iw + col] = (int16_t)(lo + t * range);
    }
  }

  // Atkinson error diffusion (distributes 6/8 of error)
  for (int row = 0; row < ih; row++) {
    for (int col = 0; col < iw; col++) {
      int idx = row * iw + col;
      int16_t old = buf[idx];
      int16_t val = (old > 127) ? 255 : 0;
      buf[idx] = val;
      int16_t err = (old - val) / 8;

      if (col + 1 < iw)                           buf[idx + 1]      += err;
      if (col + 2 < iw)                           buf[idx + 2]      += err;
      if (row + 1 < ih) {
        if (col - 1 >= 0)                          buf[idx + iw - 1] += err;
                                                   buf[idx + iw]     += err;
        if (col + 1 < iw)                          buf[idx + iw + 1] += err;
      }
      if (row + 2 < ih)                            buf[idx + 2*iw]   += err;
    }
  }

  for (int row = 0; row < ih; row++) {
    for (int col = 0; col < iw; col++) {
      if (buf[row * iw + col] > 127) {
        c.drawPixel(x + 1 + col, y + 1 + row, GxEPD_WHITE);
      }
    }
  }

  free(buf);
}

void drawEvents()
{
  computeOverlapColumns();

  for (int i = 0; i < eventCount; i++) {
    CalEvent &ev = events[i];

    if (ev.allDay) continue;
    if (ev.endHour < HOUR_START || (ev.endHour == HOUR_START && ev.endMin == 0)) continue;
    if (ev.startHour >= HOUR_END) continue;

    int sh = ev.startHour, sm = ev.startMin;
    int eh = ev.endHour,   em = ev.endMin;
    if (sh < HOUR_START || (sh == HOUR_START && sm < 0)) { sh = HOUR_START; sm = 0; }
    if (eh > HOUR_END || (eh == HOUR_END && em > 0))     { eh = HOUR_END;   em = 0; }

    int y1 = timeToY(sh, sm);
    int y2 = timeToY(eh, em);
    int blockH = y2 - y1;
    if (blockH < 2) blockH = 2;

    int cols = evColCount[i];
    int col  = evCol[i];
    int colW = EVENT_W / cols;
    int ex   = EVENT_X + col * colW;
    int ew   = (col == cols - 1) ? (EVENT_W - col * colW) : colW;

    // Inset for gaps between adjacent blocks
    int drawX = ex + EVENT_GAP;
    int drawY = y1 + EVENT_GAP;
    int drawW = ew - EVENT_GAP * 2;
    int drawH = blockH - EVENT_GAP;
    if (drawW < 4) drawW = 4;
    if (drawH < 2) drawH = 2;

    fillRoundRectDithered(canvas, drawX, drawY, drawW, drawH, EVENT_RADIUS);

    if (drawH >= 14) {
      canvas.setFont(&FreeSans9pt7b);
      canvas.setTextColor(GxEPD_WHITE);

      char truncated[64];
      truncateToFit(ev.title, drawW - 8, truncated, sizeof(truncated));

      int16_t tx, ty;
      uint16_t tw, th;
      canvas.getTextBounds(truncated, 0, 0, &tx, &ty, &tw, &th);

      int textX = drawX + 4;
      int textY = drawY + 3 + (int)th;

      // Black outline: draw at 8 neighbouring offsets
      canvas.setTextColor(GxEPD_BLACK);
      for (int8_t dy = -2; dy <= 2; dy++) {
        for (int8_t dx = -2; dx <= 2; dx++) {
          if (dx == 0 && dy == 0) continue;
          canvas.setCursor(textX + dx, textY + dy);
          canvas.print(truncated);
        }
      }
      // White fill on top
      canvas.setTextColor(GxEPD_WHITE);
      canvas.setCursor(textX, textY);
      canvas.print(truncated);

      if (drawH >= 30) {
        char timeBuf[20];
        sprintf(timeBuf, "%d:%02d - %d:%02d",
                ev.startHour > 12 ? ev.startHour - 12 : ev.startHour, ev.startMin,
                ev.endHour > 12 ? ev.endHour - 12 : ev.endHour, ev.endMin);

        int timeX = drawX + 4;
        int timeY = textY + 4;
        canvas.setFont();
        canvas.setTextSize(1);

        canvas.setTextColor(GxEPD_BLACK);
        for (int8_t dy = -1; dy <= 1; dy++) {
          for (int8_t dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            canvas.setCursor(timeX + dx, timeY + dy);
            canvas.print(timeBuf);
          }
        }
        canvas.setTextColor(GxEPD_WHITE);
        canvas.setCursor(timeX, timeY);
        canvas.print(timeBuf);
      }

      canvas.setTextColor(GxEPD_BLACK);
    }
  }

  // All-day events — drawn AFTER timed events so they sit below.
  // Actually, draw all-day banners first to reserve space and pushed down.
  // (This section intentionally left here but the actual drawing is in
  //  drawAllDayBanners() called before drawHourGrid / drawEvents.)
}

// Draw all-day banners at top of timeline and push TIMELINE_Y / TIMELINE_H down.
void drawAllDayBanners()
{
  bool hasAllDay = false;
  for (int i = 0; i < eventCount; i++) {
    if (events[i].allDay) { hasAllDay = true; break; }
  }
  if (!hasAllDay) return;

  int bannerY = HEADER_H;
  canvas.setFont(&FreeSans9pt7b);
  canvas.setTextColor(GxEPD_BLACK);

  for (int i = 0; i < eventCount; i++) {
    if (!events[i].allDay) continue;

    int16_t tx, ty;
    uint16_t tw, th;
    canvas.getTextBounds(events[i].title, 0, 0, &tx, &ty, &tw, &th);

    int bannerH = (int)th + 6;
    canvas.drawRoundRect(EVENT_X, bannerY + 2, EVENT_W, bannerH, 3, GxEPD_BLACK);
    canvas.setCursor(EVENT_X + 4, bannerY + 2 + (int)th + 2);
    canvas.print(events[i].title);
    bannerY += bannerH + 4;
  }

  // Shift timeline down below the banners
  TIMELINE_Y = bannerY + 2;
  TIMELINE_H = FOOTER_Y - TIMELINE_Y;
}

void drawNowIndicator()
{
  int hour = simTime.tm_hour;
  int minute = simTime.tm_min;

  if (hour < HOUR_START || hour >= HOUR_END) return;

  int y = timeToY(hour, minute);

  // Format current time for the pill
  char timeBuf[8];
  int dispH = hour > 12 ? hour - 12 : (hour == 0 ? 12 : hour);
  sprintf(timeBuf, "%d:%02d", dispH, minute);

  // Pill sized to text, right-aligned in gutter
  canvas.setFont();
  canvas.setTextSize(1);
  int tw = strlen(timeBuf) * 6;
  int pillPad = 4;
  int pillH = 12;
  int pillW = tw + pillPad * 2;
  int pillX = LABEL_W - pillW + 6;
  int pillY = y - pillH / 2;
  int pillR = 3;

  // Clear gutter area behind the pill to cover hour labels
  canvas.fillRect(0, pillY, LABEL_W, pillH, GxEPD_WHITE);

  // Draw horizontal line across the timeline area
  canvas.drawFastHLine(LABEL_W, y, SCREEN_W - LABEL_W - 2, GxEPD_BLACK);

  // Draw the pill (black rounded rect with white text)
  canvas.fillRoundRect(pillX, pillY, pillW, pillH, pillR, GxEPD_BLACK);
  canvas.setTextColor(GxEPD_WHITE);
  canvas.setCursor(pillX + pillPad, pillY + 2);
  canvas.print(timeBuf);

  canvas.setTextColor(GxEPD_BLACK);
}

void drawFooter()
{
  canvas.drawFastHLine(0, FOOTER_Y, SCREEN_W, GxEPD_BLACK);

  char buf[30];
  strftime(buf, sizeof(buf), "Updated %l:%M %p", &simTime);

  canvas.setFont();
  canvas.setTextSize(1);
  canvas.setTextColor(GxEPD_BLACK);
  canvas.setCursor(4, FOOTER_Y + 5);
  canvas.print(buf);

  char evBuf[16];
  sprintf(evBuf, "%d events", eventCount);
  int evW = strlen(evBuf) * 6;
  canvas.setCursor(SCREEN_W - evW - 4, FOOTER_Y + 5);
  canvas.print(evBuf);
}

// ============================================================
// Sorting (same insertion sort as firmware)
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
// BMP writer (1-bit monochrome)
// ============================================================

static void write16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void write32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

bool writeBMP(const char *path)
{
  int rowBytes = (SCREEN_W + 7) / 8;           // canvas row size
  int bmpRowBytes = ((SCREEN_W + 31) / 32) * 4; // BMP row (4-byte aligned)
  int pixelDataSize = bmpRowBytes * SCREEN_H;
  int paletteSize = 2 * 4;  // 2 RGBX entries
  int headerSize = 14 + 40 + paletteSize;

  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return false; }

  // BMP File Header (14 bytes)
  fputc('B', f); fputc('M', f);
  write32(f, headerSize + pixelDataSize);
  write16(f, 0); write16(f, 0);  // reserved
  write32(f, headerSize);

  // DIB Header — BITMAPINFOHEADER (40 bytes)
  write32(f, 40);
  write32(f, SCREEN_W);
  write32(f, SCREEN_H);   // positive = bottom-up
  write16(f, 1);           // planes
  write16(f, 1);           // bits per pixel
  write32(f, 0);           // compression (BI_RGB)
  write32(f, pixelDataSize);
  write32(f, 3780);        // X pixels/meter (~96 DPI)
  write32(f, 3780);        // Y pixels/meter
  write32(f, 2);           // colors used
  write32(f, 2);           // important colors

  // Color table: index 0 = BLACK, index 1 = WHITE
  // (GFXcanvas1: bit=0 → color 0, bit=1 → color 1)
  uint8_t black[4] = {0, 0, 0, 0};
  uint8_t white[4] = {255, 255, 255, 0};
  fwrite(black, 4, 1, f);
  fwrite(white, 4, 1, f);

  // Pixel data — BMP stores rows bottom-up
  const uint8_t *buf = canvas.getBuffer();
  uint8_t padRow[4] = {0};
  int padBytes = bmpRowBytes - rowBytes;

  for (int y = SCREEN_H - 1; y >= 0; y--) {
    fwrite(&buf[y * rowBytes], 1, rowBytes, f);
    if (padBytes > 0) fwrite(padRow, 1, padBytes, f);
  }

  fclose(f);
  return true;
}

// ============================================================
// JSON event loading (minimal parser for Apps Script format)
// ============================================================

// Skip whitespace
static const char *skipWS(const char *p) {
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

// Extract a JSON string value (between quotes). Returns pointer past closing quote.
static const char *extractString(const char *p, char *out, int maxLen) {
  p = skipWS(p);
  if (*p != '"') { out[0] = '\0'; return p; }
  p++; // skip opening "
  int i = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && *(p+1)) { p++; } // skip escape
    if (i < maxLen - 1) out[i++] = *p;
    p++;
  }
  out[i] = '\0';
  if (*p == '"') p++; // skip closing "
  return p;
}

static void addEventFromJSON(const char *title, const char *start, const char *end,
                             bool allDay, int tzOffsetHours)
{
  if (eventCount >= MAX_EVENTS) return;
  CalEvent &ce = events[eventCount];
  strncpy(ce.title, title, sizeof(ce.title) - 1);
  ce.title[sizeof(ce.title) - 1] = '\0';
  ce.allDay = allDay;

  if (allDay) {
    ce.startHour = HOUR_START; ce.startMin = 0;
    ce.endHour   = HOUR_END;   ce.endMin   = 0;
  } else {
    // Parse ISO 8601 "2026-04-15T15:00:00.000Z"
    int Y, M, D, h, m;
    if (sscanf(start, "%4d-%2d-%2dT%2d:%2d", &Y, &M, &D, &h, &m) == 5) {
      // Apply UTC→local offset
      struct tm utc = {};
      utc.tm_year = Y - 1900; utc.tm_mon = M - 1; utc.tm_mday = D;
      utc.tm_hour = h; utc.tm_min = m;
      time_t epoch = timegm(&utc) + tzOffsetHours * 3600;
      struct tm loc;
      gmtime_r(&epoch, &loc);
      ce.startHour = loc.tm_hour;
      ce.startMin  = loc.tm_min;
    }
    if (sscanf(end, "%4d-%2d-%2dT%2d:%2d", &Y, &M, &D, &h, &m) == 5) {
      struct tm utc = {};
      utc.tm_year = Y - 1900; utc.tm_mon = M - 1; utc.tm_mday = D;
      utc.tm_hour = h; utc.tm_min = m;
      time_t epoch = timegm(&utc) + tzOffsetHours * 3600;
      struct tm loc;
      gmtime_r(&epoch, &loc);
      ce.endHour = loc.tm_hour;
      ce.endMin  = loc.tm_min;
    }
  }
  eventCount++;
}

bool loadEventsFromFile(const char *path, int tzOffsetHours)
{
  FILE *f = fopen(path, "r");
  if (!f) { perror(path); return false; }

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);

  char *data = (char *)malloc(sz + 1);
  if (!data) { fclose(f); return false; }
  fread(data, 1, sz, f);
  data[sz] = '\0';
  fclose(f);

  const char *p = data;
  p = skipWS(p);
  if (*p != '[') { free(data); return false; }
  p++;

  while (*p) {
    p = skipWS(p);
    if (*p == ']') break;
    if (*p == ',') { p++; continue; }
    if (*p != '{') { p++; continue; }
    p++; // skip {

    char title[60] = {};
    char start[40] = {};
    char end[40] = {};
    bool allDay = false;

    while (*p && *p != '}') {
      p = skipWS(p);
      if (*p == ',') { p++; continue; }
      if (*p != '"') { p++; continue; }

      char key[20] = {};
      p = extractString(p, key, sizeof(key));
      p = skipWS(p);
      if (*p == ':') p++;
      p = skipWS(p);

      if (strcmp(key, "title") == 0) {
        p = extractString(p, title, sizeof(title));
      } else if (strcmp(key, "start") == 0) {
        p = extractString(p, start, sizeof(start));
      } else if (strcmp(key, "end") == 0) {
        p = extractString(p, end, sizeof(end));
      } else if (strcmp(key, "allDay") == 0) {
        if (strncmp(p, "true", 4) == 0) { allDay = true; p += 4; }
        else if (strncmp(p, "false", 5) == 0) { allDay = false; p += 5; }
      } else {
        // skip value (string, number, bool, null)
        if (*p == '"') { char tmp[256]; p = extractString(p, tmp, sizeof(tmp)); }
        else { while (*p && *p != ',' && *p != '}') p++; }
      }
    }
    if (*p == '}') p++;

    addEventFromJSON(title, start, end, allDay, tzOffsetHours);
  }

  free(data);
  return true;
}

void loadSampleEvents(int tzOffsetHours)
{
  struct { const char *title; const char *start; const char *end; bool allDay; } samples[] = {
    {"Standup",          "2026-04-15T15:00:00.000Z", "2026-04-15T15:30:00.000Z", false},
    {"Sprint Planning",  "2026-04-15T16:00:00.000Z", "2026-04-15T17:00:00.000Z", false},
    {"Lunch w/ Sarah",   "2026-04-15T18:30:00.000Z", "2026-04-15T19:30:00.000Z", false},
    {"1:1 with Manager", "2026-04-15T20:00:00.000Z", "2026-04-15T20:30:00.000Z", false},
    {"Design Sync",      "2026-04-15T20:00:00.000Z", "2026-04-15T21:00:00.000Z", false},
    {"Code Review",      "2026-04-15T21:00:00.000Z", "2026-04-15T22:00:00.000Z", false},
    {"Company Holiday",  "",                          "",                          true},
  };

  for (auto &s : samples) {
    addEventFromJSON(s.title, s.start, s.end, s.allDay, tzOffsetHours);
  }
}

// ============================================================
// Main
// ============================================================

int main(int argc, char *argv[])
{
  const char *timeStr = nullptr;
  const char *eventsFile = nullptr;
  const char *outputPath = "simulator_output.bmp";
  int tzOffset = -7;  // MST default
  int scale = 1;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
      timeStr = argv[++i];
    } else if (strcmp(argv[i], "--events") == 0 && i + 1 < argc) {
      eventsFile = argv[++i];
    } else if (strcmp(argv[i], "--tz") == 0 && i + 1 < argc) {
      tzOffset = atoi(argv[++i]);
    } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
      outputPath = argv[++i];
    } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      scale = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      printf("Usage: %s [options]\n"
             "  --time HH:MM     Override current time\n"
             "  --events FILE    Load events from JSON file\n"
             "  --tz OFFSET      UTC offset in hours (default: -7 for MST)\n"
             "  -o, --output F   Output file (default: simulator_output.bmp)\n"
             "  --scale N        Scale factor (N×N nearest-neighbor)\n",
             argv[0]);
      return 0;
    }
  }

  // Load events
  if (eventsFile) {
    if (!loadEventsFromFile(eventsFile, tzOffset)) {
      fprintf(stderr, "Failed to load events from %s\n", eventsFile);
      return 1;
    }
  } else {
    loadSampleEvents(tzOffset);
    printf("Using sample events (pass --events file.json to use real data)\n");
  }
  sortEvents();

  // Determine simulated time
  time_t now = time(nullptr);
  gmtime_r(&now, &simTime);
  // Apply timezone offset
  time_t local = now + tzOffset * 3600;
  gmtime_r(&local, &simTime);

  if (timeStr) {
    int h, m;
    if (sscanf(timeStr, "%d:%d", &h, &m) == 2) {
      simTime.tm_hour = h;
      simTime.tm_min = m;
    }
  }

  char timeBuf[80];
  strftime(timeBuf, sizeof(timeBuf), "%A, %B %d  %H:%M", &simTime);
  printf("Simulating: %s  (%d events)\n", timeBuf, eventCount);

  // Reset timeline bounds to defaults (in case of re-render)
  TIMELINE_Y = HEADER_H;
  TIMELINE_H = SCREEN_H - HEADER_H - FOOTER_H;

  // Render
  canvas.fillScreen(GxEPD_WHITE);
  drawHeader();
  drawAllDayBanners();   // must come before grid/events — adjusts TIMELINE_Y
  drawHourGrid();
  drawEvents();
  drawNowIndicator();
  drawFooter();

  // Write output
  if (!writeBMP(outputPath)) {
    fprintf(stderr, "Failed to write %s\n", outputPath);
    return 1;
  }

  printf("Saved → %s  (%dx%d)\n", outputPath, SCREEN_W, SCREEN_H);
  return 0;
}
