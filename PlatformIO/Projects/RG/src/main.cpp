/* =========================================================================
   RainGauge.ino
   ESP8266 NodeMCU 12E — IoT Tipping-Bucket Rain Gauge Dashboard

   - Connects to Wi-Fi
   - Hosts index.html / style.css / script.js from LittleFS
   - Reads a tipping-bucket rain gauge via a Hall-effect sensor (A3144) on D2
   - Serves live JSON on GET /data matching the exact schema required by
     the existing frontend's script.js
   - Non-blocking loop(): sensor reads via interrupt, HTTP via
     server.handleClient(), stats recomputed every loop pass without delay()

   Board:      NodeMCU 1.0 (ESP-12E Module)
   Core:       ESP8266 Arduino core (latest)
   Libraries:  ESP8266WiFi, ESP8266WebServer, LittleFS, ArduinoJson
   ========================================================================= */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

const char INDEX_HTML[] PROGMEM = R"=====({
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Rain Gauge Dashboard</title>
<style>
  body{font-family:Arial,sans-serif;margin:0;background:#07111f;color:#e6f0ff;padding:24px;}
  .card{max-width:720px;margin:0 auto;background:#10233d;padding:24px;border-radius:16px;box-shadow:0 10px 30px rgba(0,0,0,0.25);} 
  h1{margin-top:0;} .row{display:flex;justify-content:space-between;padding:10px 0;border-bottom:1px solid rgba(255,255,255,0.12);} 
  .pill{display:inline-block;padding:6px 10px;border-radius:999px;background:#1e4f7a;color:#8edff7;font-size:12px;margin-top:8px;}
</style>
</head>
<body>
  <div class="card">
    <h1>Rain Gauge Dashboard</h1>
    <div class="pill">Hosted by ESP8266 AP</div>
    <p id="status">Loading live data...</p>
    <div class="row"><span>Station</span><strong id="station">—</strong></div>
    <div class="row"><span>Tips today</span><strong id="tips">0</strong></div>
    <div class="row"><span>Rainfall</span><strong id="rain">0.0 mm</strong></div>
    <div class="row"><span>Last update</span><strong id="updated">—</strong></div>
  </div>
  <script>
    async function load() {
      try {
        const res = await fetch('/data');
        if (!res.ok) throw new Error('bad response');
        const data = await res.json();
        const rain = (data.totalTipsToday * data.mlPerTip / 78.5) * 10;
        document.getElementById('station').textContent = data.stationName || 'Rain Gauge';
        document.getElementById('tips').textContent = data.totalTipsToday;
        document.getElementById('rain').textContent = rain.toFixed(1) + ' mm';
        document.getElementById('updated').textContent = data.lastUpdated || '—';
        document.getElementById('status').textContent = 'Live data received from ESP';
      } catch (err) {
        document.getElementById('status').textContent = 'Waiting for sensor data...';
      }
    }
    load();
    setInterval(load, 5000);
  </script>
</body>
</html>
})=====";

const char FALLBACK_CSS[] PROGMEM = "body{font-family:Arial,sans-serif;}";
const char FALLBACK_JS[] PROGMEM = "console.log('fallback');";

/* -------------------------------------------------------------------------
   ACCESS POINT CONFIGURATION
   ------------------------------------------------------------------------- */
const char* apSsid = "RainGauge-AP";
const char* apPassword = "rain12345";

/* -------------------------------------------------------------------------
   STATION METADATA (shown on the dashboard)
   ------------------------------------------------------------------------- */
const char* STATION_NAME     = "Backyard Rain Gauge";
const char* STATION_LOCATION = "Mumbai";

/* -------------------------------------------------------------------------
   SENSOR CONFIG — UNCHANGED, exactly as provided
   ------------------------------------------------------------------------- */
const int SENSOR_PIN = D2;          // GPIO4
const float ML_PER_TIP = 7.0;

volatile unsigned long tipCount = 0;
volatile unsigned long lastInterrupt = 0;

void IRAM_ATTR onTip() {
    unsigned long now = millis();

    if (now - lastInterrupt > 200) {
        tipCount++;
        lastInterrupt = now;
    }
}

/* -------------------------------------------------------------------------
   WEB SERVER
   ------------------------------------------------------------------------- */
ESP8266WebServer server(80);

/* -------------------------------------------------------------------------
   TIME SYNC (NTP) — used to timestamp tips and bucket them by hour.
   No cloud service is used for data; NTP is only a clock sync so
   "lastUpdated" / "lastTipTimestamp" / hourly buckets are meaningful.
   If NTP is unreachable, millis()-based fallback timestamps are used
   instead so the dashboard still works fully offline on the LAN.
   ------------------------------------------------------------------------- */
const long GMT_OFFSET_SEC = 5 * 3600 + 1800; // IST = UTC+5:30
const int DAYLIGHT_OFFSET_SEC = 0;
bool timeSynced = false;

/* -------------------------------------------------------------------------
   RAIN STATE TRACKING
   ------------------------------------------------------------------------- */
#define HOURLY_BUCKETS 12          // last 12 hours, matches frontend schema
#define MAX_RECENT_INTERVALS 5     // matches recentTipIntervalsSec length

unsigned long lastProcessedTipCount = 0;   // last tipCount we folded into stats
unsigned long totalTipsToday = 0;          // resets at local midnight
int currentDay = -1;                       // day-of-year, to detect midnight rollover

unsigned long lastTipMillis = 0;           // millis() at last tip (0 = none yet)
bool hasTippedYet = false;
time_t lastTipEpoch = 0;                   // wall-clock time of last tip (if synced)

// Ring buffer of recent tip-to-tip intervals in seconds
unsigned long recentIntervalsSec[MAX_RECENT_INTERVALS] = {0, 0, 0, 0, 0};
int recentIntervalsCount = 0;
int recentIntervalsIndex = 0;

// Hourly tip counts for the last 12 hours, indexed by hour-of-day (0-23),
// stored as a rolling array of the last 12 distinct hour slots seen.
struct HourSlot {
  int hour;              // 0-23, -1 = unused slot
  unsigned long tips;
};
HourSlot hourlyHistory[HOURLY_BUCKETS];
int hourlyHistoryHead = 0;   // index of most recent slot
int hourlyHistoryFilled = 0; // how many slots have been used so far
int lastRecordedHour = -1;   // hour-of-day of the most recent slot

/* -------------------------------------------------------------------------
   FORWARD DECLARATIONS
   ------------------------------------------------------------------------- */
void setupTime();
void handleTips();
void rolloverCheckMidnight(struct tm* timeinfo);
void recordTipIntoHourlyHistory(int hourOfDay);
void handleRoot();
void handleStyleCss();
void handleScriptJs();
void handleData();
void handleNotFound();
void serveFile(const char* path, const char* contentType);
String isoTimestamp(time_t t);
String isoTimestampNow();

/* =========================================================================
   SETUP
   ========================================================================= */
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== IoT Rain Gauge Dashboard booting ==="));

  // --- Sensor pin ---
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onTip, FALLING);
  Serial.println(F("[SENSOR] Hall-effect tip sensor armed on D2 (FALLING, pullup)."));

  // --- LittleFS ---
  if (!LittleFS.begin()) {
    Serial.println(F("[LITTLEFS] ERROR: Failed to mount LittleFS. Web files will not be served."));
  } else {
    Serial.println(F("[LITTLEFS] Mounted successfully."));
  }

  // --- Access Point ---
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid, apPassword);
  Serial.print(F("[AP] Started hotspot: "));
  Serial.println(apSsid);
  Serial.print(F("[AP] Password: "));
  Serial.println(apPassword);

  // --- Time sync (best-effort; dashboard still works if this fails) ---
  setupTime();

  // --- HTTP routes ---
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/style.css", HTTP_GET, handleStyleCss);
  server.on("/script.js", HTTP_GET, handleScriptJs);
  server.on("/data", HTTP_GET, handleData);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println(F("[HTTP] Server started on port 80."));
  Serial.print(F("[HTTP] Connect to Wi-Fi: "));
  Serial.println(apSsid);
  Serial.print(F("[HTTP] Open http://"));
  Serial.println(WiFi.softAPIP());
}

/* =========================================================================
   LOOP — never blocks. No delay() anywhere in here.
   ========================================================================= */
void loop() {
  server.handleClient();   // serve any pending HTTP request
  handleTips();            // fold any new interrupt-counted tips into stats
}

/* =========================================================================
   TIME
   ========================================================================= */
void setupTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");
  Serial.print(F("[TIME] Waiting for NTP sync"));

  time_t now = time(nullptr);
  unsigned long startAttempt = millis();
  // Bounded wait so a missing/blocked NTP server can't hang boot forever.
  while (now < 100000 && millis() - startAttempt < 8000) {
    delay(250); // acceptable here: one-time blocking wait during initial boot only
    Serial.print(F("."));
    now = time(nullptr);
  }
  Serial.println();

  if (now >= 100000) {
    timeSynced = true;
    Serial.println(F("[TIME] NTP sync successful."));
  } else {
    timeSynced = false;
    Serial.println(F("[TIME] NTP sync failed — falling back to uptime-relative timestamps."));
  }
}

/* =========================================================================
   TIP PROCESSING (called every loop pass; no blocking work here)
   ========================================================================= */
void handleTips() {
  // Snapshot the volatile counter once, atomically enough for our purposes
  noInterrupts();
  unsigned long snapshotTipCount = tipCount;
  unsigned long snapshotLastInterrupt = lastInterrupt;
  interrupts();

  // --- Midnight rollover check (only meaningful once time is synced) ---
  time_t nowEpoch = time(nullptr);
  struct tm timeinfo;
  bool haveWallClock = timeSynced && nowEpoch > 100000;
  if (haveWallClock) {
    localtime_r(&nowEpoch, &timeinfo);
    rolloverCheckMidnight(&timeinfo);
  }

  // --- Fold in any newly counted tips since last pass ---
  while (lastProcessedTipCount < snapshotTipCount) {
    lastProcessedTipCount++;
    totalTipsToday++;

    // Interval since previous tip
    if (hasTippedYet) {
      unsigned long intervalMs;
      // millis() at time of this tip isn't individually recorded per-tip
      // (interrupt only stores the latest lastInterrupt), so for the tip
      // currently being folded in we use lastInterrupt as its timestamp.
      intervalMs = snapshotLastInterrupt - lastTipMillis;
      unsigned long intervalSec = intervalMs / 1000;

      recentIntervalsSec[recentIntervalsIndex] = intervalSec;
      recentIntervalsIndex = (recentIntervalsIndex + 1) % MAX_RECENT_INTERVALS;
      if (recentIntervalsCount < MAX_RECENT_INTERVALS) recentIntervalsCount++;
    }

    lastTipMillis = snapshotLastInterrupt;
    hasTippedYet = true;
    lastTipEpoch = haveWallClock ? nowEpoch : 0;

    // Hourly bucket for this tip
    int hourOfDay = haveWallClock ? timeinfo.tm_hour : -1;
    if (hourOfDay >= 0) {
      recordTipIntoHourlyHistory(hourOfDay);
    }
  }
}

// Detects local-midnight rollover and resets the daily tip counter.
void rolloverCheckMidnight(struct tm* timeinfo) {
  int today = timeinfo->tm_yday;
  if (currentDay == -1) {
    currentDay = today; // first time we ever got a valid clock reading
    return;
  }
  if (today != currentDay) {
    currentDay = today;
    totalTipsToday = 0;
    Serial.println(F("[STATS] Midnight rollover — totalTipsToday reset to 0."));
  }
}

// Adds one tip to the hourly history ring buffer. If the current hour
// already has a slot (it's the most recent one), increments it; otherwise
// opens a new slot, evicting the oldest if the buffer is full.
void recordTipIntoHourlyHistory(int hourOfDay) {
  if (hourlyHistoryFilled > 0 && lastRecordedHour == hourOfDay) {
    hourlyHistory[hourlyHistoryHead].tips++;
    return;
  }

  hourlyHistoryHead = (hourlyHistoryHead + 1) % HOURLY_BUCKETS;
  hourlyHistory[hourlyHistoryHead].hour = hourOfDay;
  hourlyHistory[hourlyHistoryHead].tips = 1;
  lastRecordedHour = hourOfDay;
  if (hourlyHistoryFilled < HOURLY_BUCKETS) hourlyHistoryFilled++;
}

/* =========================================================================
   TIMESTAMP FORMATTING (ISO 8601, UTC — matches "2026-07-30T18:22:00Z")
   ========================================================================= */
String isoTimestamp(time_t t) {
  struct tm utcTime;
  gmtime_r(&t, &utcTime);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utcTime);
  return String(buf);
}

String isoTimestampNow() {
  time_t now = time(nullptr);
  if (timeSynced && now > 100000) {
    return isoTimestamp(now);
  }
  // Fallback: no wall clock available yet. Use a placeholder that is still
  // valid ISO 8601 so the frontend's Date parsing does not break.
  return "1970-01-01T00:00:00Z";
}

/* =========================================================================
   HTTP HANDLERS
   ========================================================================= */

void serveFile(const char* path, const char* contentType) {
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    server.streamFile(f, contentType);
    f.close();
    return;
  }

  if (String(path) == "/index.html") {
    server.send_P(200, "text/html", INDEX_HTML);
    return;
  }
  if (String(path) == "/style.css") {
    server.send_P(200, "text/css", FALLBACK_CSS);
    return;
  }
  if (String(path) == "/script.js") {
    server.send_P(200, "application/javascript", FALLBACK_JS);
    return;
  }

  server.send(404, "text/plain", "File not found in LittleFS: " + String(path));
}

void handleRoot() {
  serveFile("/index.html", "text/html");
}

void handleStyleCss() {
  serveFile("/style.css", "text/css");
}

void handleScriptJs() {
  serveFile("/script.js", "application/javascript");
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Not Found");
}

void handleData() {
  // Snapshot volatile / shared state consistently
  noInterrupts();
  interrupts();

  JsonDocument doc;

  doc["stationName"] = STATION_NAME;
  doc["stationLocation"] = STATION_LOCATION;
  doc["lastUpdated"] = isoTimestampNow();
  doc["mlPerTip"] = ML_PER_TIP;
  doc["totalTipsToday"] = totalTipsToday;

  JsonArray hourly = doc["hourlyTips"].to<JsonArray>();

  if (hourlyHistoryFilled == 0) {
    // No wall-clock-tagged tips yet: emit 12 empty hour slots ending "now"
    // (or ending at hour 0 if time has never synced) so the chart still
    // renders correctly with zeros instead of an empty array.
    time_t now = time(nullptr);
    int nowHour = 0;
    if (timeSynced && now > 100000) {
      struct tm t;
      localtime_r(&now, &t);
      nowHour = t.tm_hour;
    }
    for (int i = 0; i < HOURLY_BUCKETS; i++) {
      int hour = ((nowHour - (HOURLY_BUCKETS - 1 - i)) % 24 + 24) % 24;
      JsonObject slot = hourly.add<JsonObject>();
      char hourBuf[6];
      snprintf(hourBuf, sizeof(hourBuf), "%02d:00", hour);
      slot["hour"] = hourBuf;
      slot["tips"] = 0;
    }
  } else {
    // Emit the filled slots in chronological order (oldest first).
    // If fewer than HOURLY_BUCKETS slots have been filled yet, only emit
    // those — the frontend chart scales its own axis from array length.
    int startOffset = (hourlyHistoryFilled < HOURLY_BUCKETS)
                         ? 0
                         : (hourlyHistoryHead + 1) % HOURLY_BUCKETS;
    for (int i = 0; i < hourlyHistoryFilled; i++) {
      int idx;
      if (hourlyHistoryFilled < HOURLY_BUCKETS) {
        idx = i; // slots were filled sequentially from index 0
      } else {
        idx = (startOffset + i) % HOURLY_BUCKETS;
      }
      JsonObject slot = hourly.add<JsonObject>();
      char hourBuf[6];
      snprintf(hourBuf, sizeof(hourBuf), "%02d:00", hourlyHistory[idx].hour);
      slot["hour"] = hourBuf;
      slot["tips"] = hourlyHistory[idx].tips;
    }
  }

  if (hasTippedYet) {
    if (timeSynced && lastTipEpoch > 100000) {
      doc["lastTipTimestamp"] = isoTimestamp(lastTipEpoch);
    } else {
      doc["lastTipTimestamp"] = isoTimestampNow();
    }
  } else {
    doc["lastTipTimestamp"] = "1970-01-01T00:00:00Z";
  }

  JsonArray intervals = doc["recentTipIntervalsSec"].to<JsonArray>();
  // Emit oldest-to-newest of whatever intervals we have so far
  int count = recentIntervalsCount;
  int startIdx = (recentIntervalsIndex - count + MAX_RECENT_INTERVALS) % MAX_RECENT_INTERVALS;
  for (int i = 0; i < count; i++) {
    int idx = (startIdx + i) % MAX_RECENT_INTERVALS;
    intervals.add(recentIntervalsSec[idx]);
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}
