/*
 * ESP32 serial bridge for CivSwitch
 *
 * Creates an air-gapped Wi-Fi AP and relays payloads from your PC
 * to the Switch over USB serial. Your PC stays on its normal network.
 *
 * The Switch connects to this AP. Your PC talks to the ESP32 over
 * USB serial. The ESP32 opens a TCP connection to the Switch on its
 * own network and forwards the payload + response.
 *
 * Serial protocol:
 *   PC sends:     SEND <ip> <port> <length>\n<payload bytes>
 *   ESP32 replies: console response, then \nEND\n
 *   Other commands: STATUS, SCAN
 *
 * Works with any ESP32 board (WROOM-32, S2, S3, C3, etc).
 * Compile with Arduino ESP32 board package v3.x.
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

// ── Config (change these if you want) ───────────────────────────────
const char* AP_SSID     = "Switch2Bridge";
const int   AP_CHANNEL  = 6;
const int   MAX_CLIENTS = 4;

IPAddress apIP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// Built-in LED pin. GPIO 2 on most boards.
// Set to -1 if your board doesn't have one or you don't care.
const int LED_PIN = 2;

// ── DNS + HTTP (makes the Switch accept the network) ────────────────
DNSServer dnsServer;
WebServer httpServer(80);

// ── Serial relay ────────────────────────────────────────────────────
String serialBuf = "";

// ── LED state ───────────────────────────────────────────────────────
int lastClientCount = -1;
unsigned long lastBlinkTime = 0;
int blinkPhase = 0;

// Nintendo connectivity-check handler.
// The Switch probes specific hostnames after joining a network. If the
// responses match what Nintendo's servers return, it considers itself
// online and skips the captive-portal popup entirely.
//
// Adapted from 90DNS (GPL-2.0-or-later) via
// fernando-granco/switch2-homebrew (MIT).
void handleHttp() {
  String host = httpServer.hostHeader();

  // Legacy connectivity test (conntest.nintendowifi.net)
  if (host == "conntest.nintendowifi.net") {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.sendHeader("X-Organization", "Nintendo");
    httpServer.send(200, "text/html",
      "<html><head><title>HTML Page</title></head>"
      "<body>This is test.html page</body></html>");
    return;
  }

  // Modern connectivity tests (ctest / api endpoints)
  if (host == "ctest.cdn.nintendo.net" ||
      host == "ctest.p01.ctest.srv.nintendo.net" ||
      host == "ctest-dl-lp1.cdn.nintendo.net" ||
      host == "ctest-ul-lp1.cdn.nintendo.net" ||
      host == "api.p01.lp1.ctest.srv.nintendo.net" ||
      host == "api-ipv6.p01.lp1.ctest.srv.nintendo.net") {
    httpServer.sendHeader("Cache-Control", "no-store");
    httpServer.sendHeader("X-Organization", "Nintendo");
    httpServer.send(200, "text/plain", "ok");
    return;
  }

  // Everything else — no internet forwarding
  httpServer.send(404, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(65536);

  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, gateway, subnet);

  bool ok = WiFi.softAP(AP_SSID, NULL, AP_CHANNEL, false, MAX_CLIENTS);
  if (!ok) {
    Serial.println("[ERROR] Failed to start AP");
    while (true) {
      if (LED_PIN >= 0) digitalWrite(LED_PIN, !digitalRead(LED_PIN));
      delay(100);
    }
  }

  esp_wifi_set_ps(WIFI_PS_NONE);

  // DNS catches all lookups so the Switch's connectivity check
  // resolves to us instead of timing out.
  dnsServer.start(53, "*", apIP);

  // Emulate Nintendo connectivity-check responses so the Switch
  // believes it's online and never shows a captive-portal popup.
  // Adapted from 90DNS (GPL-2.0-or-later) via fernando-granco/switch2-homebrew.
  httpServer.onNotFound(handleHttp);
  httpServer.begin();

  Serial.println();
  Serial.println("========================================");
  Serial.println("  CivSwitch Serial Bridge");
  Serial.printf("  SSID:     %s\n", AP_SSID);
  Serial.println("  Auth:     Open (no password)");
  Serial.printf("  AP IP:    %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("========================================");
  Serial.println();
  Serial.println("Connect the Switch to this AP (no popup).");
  Serial.println("Send payloads from your PC with send_serial.py.");
  Serial.println();
}

void relayPayload(const char* ip, int port, const uint8_t* payload, int len) {
  Serial.printf("[RELAY] Connecting to %s:%d (%d bytes)...\n", ip, port, len);

  WiFiClient client;
  if (!client.connect(ip, port, 8000)) {
    Serial.println("[RELAY] ERROR: connection failed");
    Serial.println("\nEND");
    return;
  }

  client.write(payload, len);
  client.flush();

  // TCP half-close — tells the loader "payload complete, run it"
  int fd = client.fd();
  if (fd >= 0) {
    ::shutdown(fd, SHUT_WR);
  }

  Serial.println("[RELAY] Sent. Reading response...");
  unsigned long deadline = millis() + 120000;
  bool gotData = false;

  while (client.connected() && millis() < deadline) {
    while (client.available()) {
      char c = client.read();
      Serial.write(c);
      gotData = true;
      deadline = millis() + 5000;
    }
    delay(10);
  }

  client.stop();
  if (!gotData) Serial.println("[RELAY] (no response)");
  Serial.println("\nEND");
}

void processSerialCommand(String& line) {
  line.trim();
  if (line.startsWith("SEND ")) {
    int i1 = 5;
    int i2 = line.indexOf(' ', i1);
    int i3 = line.indexOf(' ', i2 + 1);
    if (i2 < 0 || i3 < 0) {
      Serial.println("[ERROR] Usage: SEND <ip> <port> <length>");
      Serial.println("\nEND");
      return;
    }
    String ip = line.substring(i1, i2);
    int port = line.substring(i2 + 1, i3).toInt();
    int payloadLen = line.substring(i3 + 1).toInt();

    if (payloadLen <= 0 || payloadLen > 262144) {
      Serial.println("[ERROR] Invalid payload length");
      Serial.println("\nEND");
      return;
    }

    uint8_t* buf = (uint8_t*)malloc(payloadLen);
    if (!buf) {
      Serial.println("[ERROR] Out of memory");
      Serial.println("\nEND");
      return;
    }

    int received = 0;
    unsigned long timeout = millis() + 10000;
    while (received < payloadLen && millis() < timeout) {
      if (Serial.available()) {
        buf[received++] = Serial.read();
        timeout = millis() + 5000;
      }
    }

    if (received < payloadLen) {
      Serial.printf("[ERROR] Only got %d of %d bytes\n", received, payloadLen);
      Serial.println("\nEND");
      free(buf);
      return;
    }

    relayPayload(ip.c_str(), port, buf, payloadLen);
    free(buf);

  } else if (line == "STATUS") {
    Serial.printf("[STATUS] AP clients: %d\n", WiFi.softAPgetStationNum());
    Serial.println("\nEND");

  } else if (line == "SCAN") {
    wifi_sta_list_t staList;
    if (esp_wifi_ap_get_sta_list(&staList) == ESP_OK) {
      Serial.printf("[SCAN] %d clients\n", staList.num);
      for (int i = 0; i < staList.num; i++) {
        uint8_t* m = staList.sta[i].mac;
        Serial.printf("  %02X:%02X:%02X:%02X:%02X:%02X\n",
          m[0], m[1], m[2], m[3], m[4], m[5]);
      }
    }
    Serial.println("\nEND");

  } else if (line.length() > 0) {
    Serial.println("[ERROR] Unknown command. Use: SEND, STATUS, SCAN");
    Serial.println("\nEND");
  }
}

void loop() {
  dnsServer.processNextRequest();
  httpServer.handleClient();

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      processSerialCommand(serialBuf);
      serialBuf = "";
    } else {
      serialBuf += c;
    }
  }

  int clients = WiFi.softAPgetStationNum();
  if (clients != lastClientCount) {
    lastClientCount = clients;
    Serial.printf("[INFO] Connected clients: %d\n", clients);
  }

  if (LED_PIN >= 0) updateLED(clients);
  delay(10);
}

void updateLED(int clients) {
  if (clients == 0) {
    digitalWrite(LED_PIN, HIGH);
    return;
  }

  unsigned long now = millis();
  int totalPhases = clients * 2 + 4;

  if (now - lastBlinkTime > 200) {
    lastBlinkTime = now;
    blinkPhase = (blinkPhase + 1) % totalPhases;

    if (blinkPhase < clients * 2) {
      digitalWrite(LED_PIN, blinkPhase % 2 == 0 ? HIGH : LOW);
    } else {
      digitalWrite(LED_PIN, LOW);
    }
  }
}
