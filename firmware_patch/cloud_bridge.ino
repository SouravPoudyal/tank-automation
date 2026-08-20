/*
  cloud_bridge.ino — companion tab for BHARAT_IOT_pump_control_curve_9.ino
  ---------------------------------------------------------------------
  Add this as a SECOND TAB in the same Arduino IDE sketch folder (or a
  second file alongside the .ino if you're on PlatformIO). It does NOT
  replace anything — the existing AP-mode local dashboard at
  192.168.4.1 keeps working exactly as before. This just ALSO joins
  your home WiFi and check-ins with a Render-hosted bridge server so
  the dashboard can be reached from anywhere.

  WHAT YOU NEED TO EDIT BELOW:
    - HOME_WIFI_SSID / HOME_WIFI_PASSWORD
    - BRIDGE_HOST        (your Render service's hostname, no https://)
    - DEVICE_TOKEN        (must match the DEVICE_TOKEN env var on Render)

  ONE-TIME LIBRARY NEEDED:
    - none beyond what's already used (WiFi.h, HTTPClient, LittleFS are
      all bundled with the ESP32 Arduino core, and LittleFS is already
      used by the main sketch for its own logs/settings)

  HOW IT WORKS:
    A dedicated FreeRTOS task (bridgeTask) runs independently of the
    existing controlTask/pzemTask/logTask. Every BRIDGE_INTERVAL_MS it:
      1. Builds the same JSON buildStatusJSON() already produces for
         the local dashboard's GET /status.
      2. POSTs it to https://<BRIDGE_HOST>/api/device/checkin
      3. Reads back any pending command and raises the SAME volatile
         flags (webRequestStart, webRequestStop, webRequestReset, and
         the threshold setters) that the local /api/... routes and the
         physical button already use — so loop()/controlTask() is
         still the only place that actually mutates automation state,
         exactly per the concurrency note at the top of the main .ino.
    Separately, every BRIDGE_LOG_INTERVAL_MS it also mirrors the three
    CSV history logs (/fulllog.csv, /faultlog.csv, /pzemlog.csv) up to
    the bridge, so the Report/Plots tabs on the remote dashboard have
    something to show. Those files are kept small on purpose by the
    main sketch's own 30-entry auto-clear policy, so this comfortably
    fits in RAM — no chunking needed.

  TWO OPTIONAL HOOKS INTO THE MAIN SKETCH:
    clearAllData (remote) resets the three log files itself (pure
    LittleFS truncate, done entirely in this file), but two additional
    things the local dashboard's clearAllData also does — resetting
    the pump cycle counter and the "last full"/"last fault" quick
    fields, and persisting the phone-alerts on/off toggle — live in
    variables/functions only the main sketch knows the real names of.
    Both hooks below are declared __attribute__((weak)) with a no-op
    default, so this file compiles and runs standalone without any
    edits to the main sketch. If you want those two things to fully
    work when triggered remotely, add matching STRONG (non-weak)
    definitions with these exact names/signatures to the main sketch —
    see the stub bodies near the bottom of this file for what each one
    should do.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>

// ---------------- EDIT THESE ----------------
const char* HOME_WIFI_SSID     = "YOUR_HOME_WIFI_NAME";
const char* HOME_WIFI_PASSWORD = "YOUR_HOME_WIFI_PASSWORD";
const char* BRIDGE_HOST        = "your-app-name.onrender.com"; // no https://, no trailing slash
const char* BRIDGE_DEVICE_TOKEN = "change-me-device-secret";   // must match Render's DEVICE_TOKEN
// ---------------------------------------------

const unsigned long BRIDGE_INTERVAL_MS = 4000;
const unsigned long BRIDGE_LOG_INTERVAL_MS = 20000; // logs are small; this cadence is plenty
const size_t BRIDGE_LOG_MAX_BYTES = 24 * 1024; // safety cap per file; auto-clear-at-30-rows keeps real files well under this

TaskHandle_t bridgeTaskHandle = NULL;

// buildStatusJSON() and the webRequest* flags are defined in the main
// .ino file; forward-declare what we need from here.
void buildStatusJSON(char* buf, size_t bufSize);
extern volatile bool webRequestStart;
extern volatile bool webRequestStop;
extern volatile bool webRequestReset;
extern float dryRunThresholdA;
extern float dryRunDefaultA;
extern bool rtcPresent;
extern SemaphoreHandle_t i2cMutex;
extern RTC_DS3231 rtc; // matches the rtc object declared in the main .ino
void saveThreshold(float v);
void saveThresholdDefault(float v);

// ---- Optional hooks (see header comment above) ----
// Default no-op implementations so this file always compiles/links on
// its own. Add real, non-weak versions of these two in the main sketch
// if you want clearAllData/alerts to fully take effect when triggered
// from the remote dashboard.
void __attribute__((weak)) resetPumpCountersForClearAllData() {
  // Real implementation should: reset the pump cycle counter to 0, and
  // clear whatever "last full" / "last fault" fields buildStatusJSON()
  // reports (mirroring exactly what the local /api/clearAllData route
  // already does today).
  Serial.println("Bridge: resetPumpCountersForClearAllData() not implemented in main sketch — cycle counter / last-full / last-fault NOT reset. See firmware_patch/cloud_bridge.ino header comment.");
}
void __attribute__((weak)) setAlertsEnabledForBridge(bool on) {
  // Real implementation should persist `on` the same way the local
  // /api/alerts route does, so buildStatusJSON()'s alertsEnabled field
  // (which the dashboard reads back to sync its toggle) reflects it.
  Serial.print("Bridge: setAlertsEnabledForBridge(");
  Serial.print(on ? "true" : "false");
  Serial.println(") not implemented in main sketch — alerts preference NOT persisted. See firmware_patch/cloud_bridge.ino header comment.");
}

void bridge_connectWifiStation() {
  // WIFI_AP_STA keeps the existing local hotspot (WIFI_AP, set up
  // elsewhere in setup()) alive WHILE ALSO joining the home network —
  // change to WIFI_STA only if you want to drop the local AP entirely.
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);

  Serial.print("Bridge: connecting to home WiFi \"");
  Serial.print(HOME_WIFI_SSID);
  Serial.print("\"");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Bridge: connected, station IP = ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Bridge: could not join home WiFi within 15s — will keep retrying in the background task.");
  }
}

// ------------------------------------------------------------------
// Log file paths + the CSV header each one is reset to on clearLog.
// Paths match exactly what the main sketch's local web server already
// serves at GET /fulllog.csv etc, and what the dashboard's own
// logDefs{} expects the columns to be.
// ------------------------------------------------------------------
struct BridgeLogDef { const char* key; const char* path; const char* header; };
const BridgeLogDef BRIDGE_LOG_DEFS[3] = {
  { "full",  "/fulllog.csv",  "cycle_number,start,stop,runtime_s,energy_kwh,avg_pf,stop_reason\n" },
  { "fault", "/faultlog.csv", "cycle_number,timestamp,fault_type\n" },
  { "pzem",  "/pzemlog.csv",  "timestamp,voltage,current,power,energy,frequency,pf\n" },
};

const BridgeLogDef* bridge_findLogDef(const String& key) {
  for (int i = 0; i < 3; i++) {
    if (key == BRIDGE_LOG_DEFS[i].key) return &BRIDGE_LOG_DEFS[i];
  }
  return nullptr;
}

// Truncates the named log back to just its header row — same end
// result as the local dashboard's clearLog, just triggered remotely.
void bridge_clearLogFile(const String& key) {
  const BridgeLogDef* def = bridge_findLogDef(key);
  if (!def) {
    Serial.print("Bridge: clearLog got unknown log key '"); Serial.print(key); Serial.println("'");
    return;
  }
  File f = LittleFS.open(def->path, "w"); // "w" truncates
  if (!f) {
    Serial.print("Bridge: failed to open "); Serial.print(def->path); Serial.println(" for clearing");
    return;
  }
  f.print(def->header);
  f.close();
  Serial.print("Bridge: cleared "); Serial.println(def->path);
}

// Escapes a raw CSV blob into a JSON string body (quotes, backslashes,
// and newlines only — that's all this content can ever contain).
void bridge_appendJSONEscaped(String& out, const String& raw) {
  out.reserve(out.length() + raw.length() + 8);
  for (size_t i = 0; i < raw.length(); i++) {
    char c = raw.charAt(i);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': break; // drop bare CRs, \n alone is enough for the dashboard's CSV parser
      case '\t': out += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) { /* skip other control chars */ }
        else out += c;
    }
  }
}

// Tracks each file's size at last successful upload so unchanged logs
// aren't re-sent every cycle. Forcing a resync just means setting the
// tracked size back to -1.
long bridge_lastUploadedSize[3] = { -1, -1, -1 };

void bridge_uploadLogs() {
  String body = "{";
  bool any = false;

  for (int i = 0; i < 3; i++) {
    const BridgeLogDef& def = BRIDGE_LOG_DEFS[i];
    if (!LittleFS.exists(def.path)) continue;

    File f = LittleFS.open(def.path, "r");
    if (!f) continue;
    size_t sz = f.size();

    if ((long)sz == bridge_lastUploadedSize[i]) { f.close(); continue; } // unchanged, skip

    if (sz > BRIDGE_LOG_MAX_BYTES) {
      Serial.print("Bridge: "); Serial.print(def.path);
      Serial.println(" is larger than expected for an auto-clearing 30-row log — skipping upload this cycle.");
      f.close();
      continue;
    }

    String content;
    content.reserve(sz + 1);
    while (f.available()) content += (char)f.read();
    f.close();

    if (any) body += ",";
    body += "\""; body += def.key; body += "\":\"";
    bridge_appendJSONEscaped(body, content);
    body += "\"";
    any = true;

    bridge_lastUploadedSize[i] = (long)sz;
  }
  body += "}";

  if (!any) return; // nothing changed since last upload

  HTTPClient http;
  String url = String("https://") + BRIDGE_HOST + "/api/device/logs";
  if (http.begin(url)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", String("Bearer ") + BRIDGE_DEVICE_TOKEN);
    int code = http.POST((uint8_t*)body.c_str(), body.length());
    if (code != 200) {
      Serial.print("Bridge: log upload failed, HTTP "); Serial.println(code);
      // Don't mark as uploaded — we'll retry with the same content next cycle.
      for (int i = 0; i < 3; i++) { /* sizes already set above; leave as-is, harmless to re-check next time */ }
    }
    http.end();
  }
}

// Applies a command relayed from the bridge by raising the same flags
// the local API/physical button use. Runs on the bridge task, NOT on
// controlTask — this only sets flags, never touches pump state directly,
// matching the existing concurrency pattern.
// `value` arrives as the raw JSON value text, already unquoted by
// bridge_parseAndApplyCommand if it was a JSON string. Callers choose
// float vs. integer parsing per action - epoch seconds (~1.7 billion)
// lose precision as a 32-bit float, which only has ~7 significant
// digits, so setTime needs strtoul() on the raw string, not toFloat().
void bridge_applyCommand(const String& action, const String& value, bool hasValue) {
  if (action == "start") {
    webRequestStart = true;
  } else if (action == "stop") {
    webRequestStop = true;
  } else if (action == "reset") {
    webRequestReset = true;
  } else if (action == "setThreshold" && hasValue) {
    float v = value.toFloat();
    if (v > 0.0f && v < 30.0f) {
      dryRunThresholdA = v;
      saveThreshold(v);
      Serial.print("Bridge: threshold set remotely to "); Serial.println(v, 3);
    }
  } else if (action == "setThresholdDefault" && hasValue) {
    float v = value.toFloat();
    if (v > 0.0f && v < 30.0f) {
      dryRunDefaultA = v;
      saveThresholdDefault(v);
      Serial.print("Bridge: default threshold set remotely to "); Serial.println(v, 3);
    }
  } else if (action == "applyThresholdDefault") {
    dryRunThresholdA = dryRunDefaultA;
    saveThreshold(dryRunDefaultA);
    Serial.println("Bridge: live threshold reset to default (remote).");
  } else if (action == "setTime" && hasValue && rtcPresent) {
    uint32_t epoch = (uint32_t) strtoul(value.c_str(), nullptr, 10);
    if (epoch >= 946684800UL) { // sanity floor: year 2000, same guard as the local /api/setTime route
      if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        rtc.adjust(DateTime(epoch));
        xSemaphoreGive(i2cMutex);
        Serial.println("Bridge: RTC time set remotely.");
      }
    }
  } else if (action == "clearLog" && hasValue) {
    bridge_clearLogFile(value);
    // Force a resync so the bridge's mirrored copy updates promptly
    // instead of waiting up to BRIDGE_LOG_INTERVAL_MS to notice.
    const BridgeLogDef* def = bridge_findLogDef(value);
    if (def) {
      for (int i = 0; i < 3; i++) if (BRIDGE_LOG_DEFS[i].key == def->key) bridge_lastUploadedSize[i] = -1;
    }
  } else if (action == "clearAllData") {
    for (int i = 0; i < 3; i++) {
      bridge_clearLogFile(BRIDGE_LOG_DEFS[i].key);
      bridge_lastUploadedSize[i] = -1; // force resync of all three
    }
    resetPumpCountersForClearAllData();
    Serial.println("Bridge: clearAllData applied (remote).");
  } else if (action == "alerts" && hasValue) {
    bool on = (value == "1" || value == "true");
    setAlertsEnabledForBridge(on);
  }
}

// Very small hand-rolled JSON scan for {"action":"...","value":X} —
// avoids pulling in ArduinoJson just for this one small response.
// If the main sketch already links ArduinoJson elsewhere, feel free
// to swap this for a proper JSON parse instead.
void bridge_parseAndApplyCommand(const String& body) {
  int actionKey = body.indexOf("\"action\"");
  if (actionKey < 0) return; // command was null -> nothing pending

  int colon = body.indexOf(':', actionKey);
  int firstQuote = body.indexOf('"', colon);
  int secondQuote = body.indexOf('"', firstQuote + 1);
  if (firstQuote < 0 || secondQuote < 0) return;
  String action = body.substring(firstQuote + 1, secondQuote);

  bool hasValue = false;
  String value;
  int valueKey = body.indexOf("\"value\"");
  if (valueKey > 0) {
    int vColon = body.indexOf(':', valueKey);
    int vEnd = body.indexOf(',', vColon);
    int vEnd2 = body.indexOf('}', vColon);
    if (vEnd < 0 || (vEnd2 >= 0 && vEnd2 < vEnd)) vEnd = vEnd2;
    if (vColon > 0 && vEnd > vColon) {
      String vStr = body.substring(vColon + 1, vEnd);
      vStr.trim();
      // The bridge sends numeric values unquoted ("value":2.1) but
      // string values quoted ("value":"fault") - strip the quotes so
      // callers always see the bare value either way.
      if (vStr.length() >= 2 && vStr.startsWith("\"") && vStr.endsWith("\"")) {
        vStr = vStr.substring(1, vStr.length() - 1);
      }
      if (vStr != "null" && vStr.length() > 0) {
        value = vStr;
        hasValue = true;
      }
    }
  }

  bridge_applyCommand(action, value, hasValue);
}

void bridgeTask(void* pv) {
  bridge_connectWifiStation();
  unsigned long lastLogUpload = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Bridge: WiFi station dropped, reconnecting...");
      bridge_connectWifiStation();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    char buf[660];
    buildStatusJSON(buf, sizeof(buf));

    HTTPClient http;
    String url = String("https://") + BRIDGE_HOST + "/api/device/checkin";
    if (http.begin(url)) {
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Authorization", String("Bearer ") + BRIDGE_DEVICE_TOKEN);
      int code = http.POST((uint8_t*)buf, strlen(buf));
      if (code == 200) {
        String resp = http.getString();
        bridge_parseAndApplyCommand(resp);
      } else {
        Serial.print("Bridge: check-in failed, HTTP "); Serial.println(code);
      }
      http.end();
    }

    unsigned long now = millis();
    if (now - lastLogUpload >= BRIDGE_LOG_INTERVAL_MS) {
      bridge_uploadLogs();
      lastLogUpload = now;
    }

    vTaskDelay(pdMS_TO_TICKS(BRIDGE_INTERVAL_MS));
  }
}

// Call this once from the main sketch's setup(), AFTER the existing
// WiFi.softAP(...) call — e.g. right after the block that prints the
// local dashboard address (around line 3329 in the main .ino):
//
//     xTaskCreatePinnedToCore(bridgeTask, "bridgeTask", 8192, NULL, 1, &bridgeTaskHandle, 0);
//
void bridge_setup() {
  xTaskCreatePinnedToCore(bridgeTask, "bridgeTask", 8192, NULL, 1, &bridgeTaskHandle, 0);
}
