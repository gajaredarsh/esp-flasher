/**
 * ESP32 #2 — OTA Receiver
 * ================================================
 * ALWAYS include this OTA receiver code in every firmware
 * you deploy to ESP32 #2 — otherwise it loses wireless
 * update capability and requires USB to recover.
 *
 * Connects to ESP32 #1's AP: ESP32-OTA-Net
 * OTA server listens on port 8080
 */

#include <WiFi.h>
#include <Update.h>

// ── Configuration ─────────────────────────────────────────────
const char* WIFI_SSID     = "ESP32-OTA-Net";
const char* WIFI_PASSWORD = "12345678";
const int   OTA_PORT      = 8080;

// ── Your app config ───────────────────────────────────────────
c
// ──────────────────────────────────────────────────────────────

WiFiServer otaServer(OTA_PORT);

// ── WiFi ──────────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++retries > 40) {
      Serial.println("\n[WiFi] FAILED — restarting");
      delay(3000);
      ESP.restart();
    }
  }
  Serial.printf("\n[WiFi] Connected. IP: %s\n",
                WiFi.localIP().toString().c_str());
}

// ── OTA: parse header helper ──────────────────────────────────
String extractHeader(const String& line, const String& key) {
  String lower = line;
  lower.toLowerCase();
  String keyLower = key;
  keyLower.toLowerCase();
  if (lower.startsWith(keyLower + ":")) {
    String val = line.substring(key.length() + 1);
    val.trim();
    return val;
  }
  return "";
}

// ── OTA: handle one client connection ────────────────────────
void handleOTAClient(WiFiClient& client) {
  Serial.println("[OTA] Client connected");

  size_t contentLength = 0;
  String md5Expected   = "";
  bool   isUpdate      = false;
  String currentLine   = "";

  unsigned long timeout = millis() + 5000;
  while (client.connected() && millis() < timeout) {
    if (!client.available()) { delay(1); continue; }
    char c = client.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (currentLine.length() == 0) break;  // end of headers

      if (currentLine.startsWith("POST") &&
          currentLine.indexOf("/update") >= 0) isUpdate = true;

      String val;
      val = extractHeader(currentLine, "Content-Length");
      if (val.length()) contentLength = val.toInt();
      val = extractHeader(currentLine, "x-md5");
      if (val.length()) md5Expected = val;

      currentLine = "";
    } else {
      currentLine += c;
    }
  }

  // Handle /status
  if (!isUpdate) {
    String resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                  "Connection: close\r\n\r\n{\"status\":\"ready\",\"ip\":\"";
    resp += WiFi.localIP().toString();
    resp += "\",\"firmware\":\"led-blink-v2\"}";
    client.print(resp);
    client.stop();
    return;
  }

  if (contentLength == 0) {
    client.print("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
    client.stop();
    return;
  }

  Serial.printf("[OTA] Receiving %u bytes, MD5=%s\n",
                contentLength, md5Expected.c_str());

  if (md5Expected.length() > 0) Update.setMD5(md5Expected.c_str());

  if (!Update.begin(contentLength)) {
    Update.printError(Serial);
    client.print("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n");
    client.stop();
    return;
  }

  uint8_t buf[4096];
  size_t received = 0;
  unsigned long dataTimeout = millis() + 30000;

  while (received < contentLength && client.connected() &&
         millis() < dataTimeout) {
    if (!client.available()) { delay(1); continue; }
    size_t toRead = min((size_t)sizeof(buf),
                        (size_t)(contentLength - received));
    size_t n = client.read(buf, toRead);
    if (n > 0) {
      Update.write(buf, n);
      received += n;
      dataTimeout = millis() + 30000;
      Serial.printf("[OTA] %u / %u bytes\r", received, contentLength);
    }
  }
  Serial.println();

  if (!Update.end(true)) {
    Update.printError(Serial);
    client.print("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n");
    client.stop();
    return;
  }

  Serial.println("[OTA] Update complete — rebooting");
  client.print("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
               "Connection: close\r\n\r\nOK");
  client.flush();
  client.stop();
  delay(500);
  ESP.restart();
}

// ═════════════════════════════════════════════════════════════
//  YOUR APPLICATION CODE BELOW — edit freely
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(200);

  connectWiFi();

  otaServer.begin();
  Serial.printf("[OTA] Receiver ready on port %d\n", OTA_PORT);
  Serial.printf("[OTA] POST to http://%s:%d/update\n",
                WiFi.localIP().toString().c_str(), OTA_PORT);
}

void loop() {
  // ── OTA check — call every loop, non-blocking ──
  WiFiClient client = otaServer.available();
  if (client) handleOTAClient(client);

  // WiFi watchdog
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost — reconnecting");
    connectWiFi();
  }

  // ── Your app code ──────────────────────────────

  // ───────────────────────────────────────────────
}

