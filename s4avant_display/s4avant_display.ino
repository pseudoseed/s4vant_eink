#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include "qrcodegen.h"
#include "s4vant_bitmap.h"
#include "s4vant_bitmap_inverted.h"
#include <time.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

// URL to your config.json
#define CONFIG_URL \
  "https://raw.githubusercontent.com/pseudoseed/s4vant_eink/main/config.json"

// JSON buffer capacity (~6 KB)
static const size_t JSON_BUFFER_SIZE = 6 * 1024;

// Panel instantiation (landscape)
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT>
  display(GxEPD2_750_T7(5, 17, 16, 4));

// Global QR buffers
static uint8_t qrcodeBuffer[qrcodegen_BUFFER_LEN_MAX];
static uint8_t tempBuffer  [qrcodegen_BUFFER_LEN_MAX];

// App state
StaticJsonDocument<JSON_BUFFER_SIZE> doc;
String qrUrl;
time_t voteStart = 0, voteEnd = 0;
uint16_t refreshInterval = 10;    // 10 s between refreshes
size_t buildCount = 0, buildIndex = 0;
unsigned long lastRefresh = 0;

// — Helpers —
// Manual ISO8601 parse: "YYYY-MM-DDTHH:MM:SS"
time_t parseISO(const char* s) {
  int Y  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
  int Mo = (s[5]-'0')*10   + (s[6]-'0');
  int D  = (s[8]-'0')*10   + (s[9]-'0');
  int h  = (s[11]-'0')*10  + (s[12]-'0');
  int m  = (s[14]-'0')*10  + (s[15]-'0');
  int sc = (s[17]-'0')*10  + (s[18]-'0');
  struct tm tm{};
  tm.tm_year = Y - 1900;
  tm.tm_mon  = Mo - 1;
  tm.tm_mday = D;
  tm.tm_hour = h;
  tm.tm_min  = m;
  tm.tm_sec  = sc;
  tm.tm_isdst = 0;
  // interpret as UTC so that configTime offset applies correctly:
  return mktime(&tm);
}

String formatCountdown() {
  time_t now = time(nullptr);
  long rem = voteEnd - now;
  if (rem <= 0) {
    return String("Voting Closed");
  }
  int H = rem / 3600;
  int M = (rem % 3600) / 60;
  char buf[32];
  snprintf(buf, sizeof(buf),
    "Voting ends in: %02dH %02dM",
    H, M
  );
  return String(buf);
}

void drawProgressBar(int x,int y,int w,int h) {
  float pct = (voteEnd > voteStart)
    ? float(time(nullptr)-voteStart)/float(voteEnd-voteStart)
    : 1.0;
  pct = constrain(pct, 0.0, 1.0);
  display.drawRect(x,y,w,h,GxEPD_BLACK);
  display.fillRect(x,y,int(w*pct),h,GxEPD_BLACK);
}

void drawQRCode() {
  qrcodegen_encodeText(
    qrUrl.c_str(),
    tempBuffer,
    qrcodeBuffer,
    qrcodegen_Ecc_LOW,
    qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
    qrcodegen_Mask_AUTO,
    true
  );
  int sz = qrcodegen_getSize(qrcodeBuffer);
  int scale = 230/sz;
  for(int yy=0; yy<sz; yy++){
    for(int xx=0; xx<sz; xx++){
      if(qrcodegen_getModule(qrcodeBuffer, xx, yy)){
        display.fillRect(20 + xx*scale,
                         20 + yy*scale,
                         scale, scale,
                         GxEPD_BLACK);
      }
    }
  }
}

void fetchConfig() {
  Serial.println(F(">> fetchConfig"));
  WiFiClientSecure tls; tls.setInsecure();
  HTTPClient http; http.begin(tls, CONFIG_URL);
  int code = http.GET();
  Serial.printf("HTTP GET: %d\n", code);
  if (code == HTTP_CODE_OK) {
    String pl = http.getString();
    auto err = deserializeJson(doc, pl);
    if (err) {
      Serial.print(F("JSON err: ")); Serial.println(err.c_str());
    } else {
      qrUrl           = doc["qr_url"].as<String>();
      voteStart       = parseISO(doc["vote_start"].as<const char*>());
      voteEnd         = parseISO(doc["vote_end"].  as<const char*>());
      // refreshInterval = doc["refresh_interval"] | refreshInterval;
      buildCount      = doc["build_list"].is<JsonArray>()
                        ? doc["build_list"].size() : 0;
      Serial.println(F("Config OK"));
      Serial.printf("items=%d\n", buildCount);
    }
  } else {
    Serial.println(F("Config GET failed"));
  }
  http.end();
}

void renderPage() {
  // — LEFT THIRD —
  drawQRCode();

  // “SCAN TO VOTE!”
  display.setFont(&FreeSansBold12pt7b);
  display.setTextColor(GxEPD_BLACK);
  display.setTextSize(1);
  display.setCursor(35, 280);
  display.print(doc["voting_message"]["upper"].as<const char*>());

  // voting_message
  display.setFont(&FreeSansBold9pt7b);
  display.setCursor(20, 335);
  display.print(doc["voting_message"]["top"].as<const char*>());
  display.setCursor(30, 360);
  display.print(doc["voting_message"]["bottom"].as<const char*>());

  // countdown
  display.setCursor(28, 430);
  display.print(formatCountdown());
  drawProgressBar(20, 440, 220, 20);

  // — RIGHT TWO-THIRDS —
  int x0 = 280, y0 = 10;

  // Header bitmap
  display.drawBitmap(
    x0, y0,
    s4vant_bitmap_inverted,
    S4VANT_BITMAP_WIDTH,
    S4VANT_BITMAP_HEIGHT,
    GxEPD_BLACK
  );

  // Car name
  y0 += S4VANT_BITMAP_HEIGHT + 35;
  display.setTextSize(2);
  display.setCursor(x0 + 80, y0);
  display.print(doc["car_name"].as<const char*>());
  y0 += 8 * 3 + 8;

  // Nickname
  display.setCursor(x0 + 185, y0);
  display.setTextSize(1);
  display.print(F("\""));
  display.print(doc["car_nickname"].as<const char*>());
  display.print(F("\""));
  y0 += 8 * 3 + 5;
  
  // Production Number (lore)
  display.setFont(&FreeSans9pt7b);
  display.setTextSize(1);
  display.setCursor(x0 + 98, y0);
  display.print(doc["car_lore"].as<const char*>());
  y0 += 10 * 3 + 13;

  // Build list heading
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(x0 + 40, y0);
  display.print(F("Build List:"));
  y0 += 8 * 3 + 4;

  // — show 3 rotating items —
  display.setFont(&FreeSans12pt7b);
  display.setTextSize(1);
  uint8_t itemsToShow = min<size_t>(3, buildCount);
  for (uint8_t i = 0; i < itemsToShow; i++) {
    size_t idx = (buildIndex + i) % buildCount;
    const char* item = doc["build_list"][idx].as<const char*>();
    char buf[64];
    snprintf(buf, sizeof(buf), "- %s", item);
    display.setCursor(x0 + 60, y0);
    display.print(buf);
    y0 += 30;  // line spacing
  }

  // Registration
  display.setFont(&FreeSansBold12pt7b);
  display.setCursor(x0 + 40, y0 += 7);
  display.print(doc["registration"]["title"].as<const char*>());
  display.setFont(&FreeSans12pt7b);
  display.print(doc["registration"]["number"].as<const char*>());
  y0 +=  30;

  // Socials, all on one line
  if (doc["socials"].is<JsonObject>()) {
    const int16_t startX = x0 + 40;
    const int16_t y = y0;
    int16_t x = startX;

    for (JsonPair kv : doc["socials"].as<JsonObject>()) {
      const char* key   = kv.key().c_str();
      const char* value = kv.value().as<const char*>();

      // 1) Draw key in bold
      display.setFont(&FreeSansBold9pt7b);
      display.setCursor(x, y);
      display.print(key);
      display.print(F(":  "));

      // measure how wide that was
      int16_t x1, y1;
      uint16_t w, h;
      String boldPart = String(key) + ":  ";
      display.getTextBounds(boldPart, 1, 1, &x1, &y1, &w, &h);
      x += w;

      // 2) Draw value in regular
      display.setFont(&FreeSans9pt7b);
      display.setCursor(x, y);
      display.print(value);

      // measure value width + a little padding
      display.getTextBounds(value, 1, 1, &x1, &y1, &w, &h);
      x += w + 10;  // 10px space before next entry
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Wi-Fi with fallback AP
  WiFiManager wm;
  wm.autoConnect("S4AVANT-SETUP");

  // MST (UTC−7) time sync
  configTime(-7*3600, 0, "pool.ntp.org");
  struct tm tm;
  while (!getLocalTime(&tm)) delay(200);
  Serial.println(F("Time OK"));

  // Init display
  display.init();
  display.setRotation(0);

  // First full draw
  fetchConfig();
  unsigned long t0 = micros();
  display.firstPage();
  do { renderPage(); } while (display.nextPage());
  Serial.printf("Full draw: %lums\n", (micros()-t0)/1000);

  lastRefresh = millis();
}

void loop() {
  if (millis() - lastRefresh >= refreshInterval * 1000UL) {
    // advance by 3 each time
    if (buildCount) buildIndex = (buildIndex + 3) % buildCount;

    fetchConfig();

    // partial (single‐flicker) update
    unsigned long t0 = micros();
    display.setPartialWindow(0,0,display.width(),display.height());
    display.firstPage();
    do { renderPage(); } while (display.nextPage());
    Serial.printf("Partial draw: %lums\n", (micros()-t0)/1000);

    lastRefresh = millis();
  }
}
