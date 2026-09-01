#include <WiFi.h>

#define WIFI_SSID "ESP32_LINK"
#define WIFI_PASS "esp32link"
#define TCP_PORT 8080
#define BAUD 115200
#define SEND_INTERVAL_MS 1000
#define RX_LINE_MAX 200

WiFiServer server(TCP_PORT);
WiFiClient client;

bool hadClient = false;
uint32_t seq = 0;
uint32_t lastSendMs = 0;
String rxLine;

void resetSession() {
  seq = 0;
  lastSendMs = 0;
  rxLine = "";
}

void setup() {
  Serial.begin(BAUD);
  delay(200);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);

  Serial.print("AP SSID=");
  Serial.println(WIFI_SSID);
  Serial.print("AP IP=");
  Serial.println(WiFi.softAPIP());

  server.begin();
  Serial.println("waiting for STA...");
}

void loop() {
  WiFiClient incoming = server.available();
  if (incoming && incoming.connected()) {
    if (client && client.connected()) {
      client.stop();
    }
    client = incoming;
    client.setNoDelay(true);
    hadClient = true;
    resetSession();
    Serial.println("client matched");
  }

  if (!client || !client.connected()) {
    if (hadClient) {
      client.stop();
      hadClient = false;
      resetSession();
      Serial.println("waiting for STA...");
    }
    return;
  }

  while (client.available()) {
    char c = (char)client.read();
    if (c == '\n') {
      if (rxLine.endsWith("\r")) {
        rxLine.remove(rxLine.length() - 1);
      }
      if (rxLine.length() > 0) {
        Serial.print("rx: ");
        Serial.println(rxLine);
      }
      rxLine = "";
    } else {
      rxLine += c;
      if (rxLine.length() > RX_LINE_MAX) {
        rxLine = "";
      }
    }
  }

  uint32_t now = millis();
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    seq++;
    String line = "AP seq=" + String(seq) + " uptime=" + String(now) + "ms";
    Serial.print("tx: ");
    Serial.println(line);
    client.print(line);
    client.print('\n');
  }
}
