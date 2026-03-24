/**
 * ESP32 #1 — OTA Sender / Controller (raw TCP push)
 */

#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <MD5Builder.h>

// ── Configuration ─────────────────────────────────────────────
const char* AP_SSID       = "ESP32-OTA-Net";
const char* AP_PASSWORD   = "12345678";
const char* RECEIVER_IP   = "192.168.4.2";
const int   RECEIVER_PORT = 8080;
const char* RECEIVER_PATH = "/update";
const char* FIRMWARE_PATH = "/firmware.bin";
const int   TRIGGER_PIN   = 0;
// ──────────────────────────────────────────────────────────────

WebServer server(80);
bool otaInProgress = false;

// ── MD5 ───────────────────────────────────────────────────────
String computeMD5(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  MD5Builder md5;
  md5.begin();
  uint8_t buf[512];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    md5.add(buf, n);
  }
  f.close();
  md5.calculate();
  return md5.toString();
}

// ── OTA Push via raw TCP ──────────────────────────────────────
bool pushOTA() {
  if (otaInProgress) {
    Serial.println("[OTA] Already running — skipped");
    return false;
  }
  otaInProgress = true;

  if (!LittleFS.exists(FIRMWARE_PATH)) {
    Serial.printf("[OTA] ERROR: %s not found\n", FIRMWARE_PATH);
    otaInProgress = false;
    return false;
  }

  // File size + MD5
  File f = LittleFS.open(FIRMWARE_PATH, "r");
  size_t fsize = f.size();
  f.close();
  Serial.printf("[OTA] Firmware: %u bytes\n", fsize);

  String md5 = computeMD5(FIRMWARE_PATH);
  Serial.printf("[OTA] MD5: %s\n", md5.c_str());

  // Connect
  WiFiClient client;
  Serial.printf("[OTA] Connecting to %s:%d\n", RECEIVER_IP, RECEIVER_PORT);
  if (!client.connect(RECEIVER_IP, RECEIVER_PORT)) {
    Serial.println("[OTA] ERROR: TCP connect failed");
    otaInProgress = false;
    return false;
  }
  client.setTimeout(60);  // seconds
  Serial.println("[OTA] Connected — sending HTTP POST headers");

  // Send HTTP POST headers manually
  client.printf("POST %s HTTP/1.1\r\n", RECEIVER_PATH);
  client.printf("Host: %s:%d\r\n", RECEIVER_IP, RECEIVER_PORT);
  client.println("Content-Type: application/octet-stream");
  client.printf("Content-Length: %u\r\n", fsize);
  client.printf("x-md5: %s\r\n", md5.c_str());
  client.println("Connection: close");
  client.println();  // end of headers

  // Stream firmware in 4 KB chunks
  f = LittleFS.open(FIRMWARE_PATH, "r");
  uint8_t buf[4096];
  size_t sent = 0;
  while (f.available()) {
    size_t toRead = min((size_t)sizeof(buf), (size_t)f.available());
    size_t n = f.read(buf, toRead);
    size_t written = client.write(buf, n);
    if (written != n) {
      Serial.println("[OTA] ERROR: write mismatch — connection dropped?");
      f.close();
      client.stop();
      otaInProgress = false;
      return false;
    }
    sent += written;
    Serial.printf("[OTA] Sent %u / %u bytes\r", sent, fsize);
  }
  f.close();
  Serial.printf("\n[OTA] All %u bytes sent — waiting for response\n", sent);

  // Wait for HTTP response (up to 10 s)
  unsigned long timeout = millis() + 10000;
  while (client.available() == 0) {
    if (millis() > timeout) {
      Serial.println("[OTA] ERROR: response timeout");
      client.stop();
      otaInProgress = false;
      return false;
    }
    delay(10);
  }

  // Read status line
  String statusLine = client.readStringUntil('\n');
  statusLine.trim();
  Serial.printf("[OTA] Response: %s\n", statusLine.c_str());
  client.stop();

  bool success = statusLine.indexOf("200") >= 0;
  if (success) {
    Serial.println("[OTA] SUCCESS — receiver rebooting into new firmware");
  } else {
    Serial.println("[OTA] FAILED — receiver rejected the update");
  }

  otaInProgress = false;
  return success;
}

// ── HTTP handlers ─────────────────────────────────────────────
void handleTrigger() {
  server.send(200, "text/plain", "OTA push started");
  pushOTA();
}

void handleStatus() {
  bool hasFile = LittleFS.exists(FIRMWARE_PATH);
  String md5 = hasFile ? computeMD5(FIRMWARE_PATH) : "none";
  String json = "{\"status\":\"ready\",\"firmware_loaded\":";
  json += hasFile ? "true" : "false";
  json += ",\"md5\":\"" + md5 + "\"}";
  server.send(200, "application/json", json);
}

void handleUploadFinish() {
  server.send(200, "text/plain", "OK");
}

void handleUploadData() {
  HTTPUpload& upload = server.upload();
  static File uploadFile;

  if (upload.status == UPLOAD_FILE_START) {
    Serial.println("[Upload] Starting...");
    LittleFS.remove(FIRMWARE_PATH);
    uploadFile = LittleFS.open(FIRMWARE_PATH, "w");
    if (!uploadFile) Serial.println("[Upload] ERROR: cannot open file");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      size_t w = uploadFile.write(upload.buf, upload.currentSize);
      if (w != upload.currentSize) Serial.println("[Upload] write mismatch");
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.flush();
      uploadFile.close();
      Serial.printf("[Upload] Done — %u bytes\n", upload.totalSize);
    }
  }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 #1 OTA Sender ===");

  pinMode(TRIGGER_PIN, INPUT_PULLUP);

  if (!LittleFS.begin(true)) {
    LittleFS.format();
    LittleFS.begin();
  }
  Serial.println("[FS] LittleFS ready");

  // List files
  File root = LittleFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.printf("  [FS] %s  %u bytes\n", file.name(), file.size());
    file = root.openNextFile();
  }

  // AP mode only
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("[AP] SSID=%s  IP=%s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());

  server.on("/trigger", HTTP_GET, handleTrigger);
  server.on("/status",  HTTP_GET, handleStatus);
  server.on("/upload",  HTTP_POST, handleUploadFinish, handleUploadData);
  server.begin();
  Serial.println("[HTTP] Server ready on port 80");
  Serial.printf("[HTTP] http://%s/trigger\n",
                WiFi.softAPIP().toString().c_str());
}

// ── Loop ──────────────────────────────────────────────────────
void loop() {
  server.handleClient();

  static unsigned long pressStart = 0;
  if (digitalRead(TRIGGER_PIN) == LOW) {
    if (pressStart == 0) pressStart = millis();
    if (millis() - pressStart > 500) {
      pressStart = 0;
      Serial.println("[Trigger] Button — starting OTA push");
      pushOTA();
    }
  } else {
    pressStart = 0;
  }
}

