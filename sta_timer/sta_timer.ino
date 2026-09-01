#include <WiFi.h>

#define WIFI_SSID "ESP32_LINK"
#define WIFI_PASS "esp32link"
#define TCP_PORT 8080
#define BAUD 115200
#define SEND_INTERVAL_MS 1000
#define WIFI_RETRY_MS 500
#define TCP_RETRY_MS 1000
#define RX_LINE_MAX 200

static const IPAddress kApIp(192, 168, 4, 1);

WiFiClient client;

bool wifiWasConnected = false;
bool tcpAnnounced = false;
uint32_t seq = 0;
uint32_t lastSendMs = 0;
uint32_t lastWifiLogMs = 0;
uint32_t lastTcpAttemptMs = 0;
String rxLine;

void resetSession() {
  seq = 0;
  lastSendMs = 0;
  rxLine = "";
}

void setup() {
  Serial.begin(BAUD);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.println("WiFi connecting...");
  lastWifiLogMs = millis();
}

void loop() {
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      wifiWasConnected = false;
      tcpAnnounced = false;
      client.stop();
      resetSession();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    }
    if (now - lastWifiLogMs >= WIFI_RETRY_MS) {
      lastWifiLogMs = now;
      Serial.println("WiFi connecting...");
    }
    return;
  }

  if (!wifiWasConnected) {
    wifiWasConnected = true;
    Serial.print("WiFi IP=");
    Serial.println(WiFi.localIP());
  }

  if (!client.connected()) {
    if (tcpAnnounced) {
      tcpAnnounced = false;
      client.stop();
      resetSession();
    }
    if (now - lastTcpAttemptMs >= TCP_RETRY_MS) {
      lastTcpAttemptMs = now;
      if (client.connect(kApIp, TCP_PORT)) {
        client.setNoDelay(true);
        resetSession();
        tcpAnnounced = true;
        Serial.println("tcp connected");
      }
    }
    if (!client.connected()) {
      return;
    }
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

  now = millis();
  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = now;
    seq++;
    String line = "STA seq=" + String(seq) + " uptime=" + String(now) + "ms";
    Serial.print("tx: ");
    Serial.println(line);
    client.print(line);
    client.print('\n');
  }
}
