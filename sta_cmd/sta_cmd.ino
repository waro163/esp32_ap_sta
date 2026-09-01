#include <WiFi.h>

#define WIFI_SSID "ESP32_LINK"
#define WIFI_PASS "esp32link"
#define TCP_PORT 8080
#define BAUD 115200
#define SEND_INTERVAL_MS 1000
#define WIFI_RETRY_MS 500
#define TCP_RETRY_MS 1000
#define LED_PIN 2
#define LED_ON HIGH
#define LED_OFF LOW

#define HDR0 0xAA
#define HDR1 0x55
#define CMD_PING 0x01
#define CMD_PONG 0x02
#define CMD_SET_LED 0x03
#define CMD_LED_ACK 0x04
#define MAX_PAYLOAD 32
#define SERIAL_LINE_MAX 64

static const IPAddress kApIp(192, 168, 4, 1);

WiFiClient client;

bool wifiWasConnected = false;
bool tcpAnnounced = false;
uint32_t lastPingMs = 0;
uint32_t lastWifiLogMs = 0;
uint32_t lastTcpAttemptMs = 0;
String serialLine;

enum ParseState {
  WAIT_H0,
  WAIT_H1,
  WAIT_CMD,
  WAIT_LEN,
  WAIT_PAYLOAD,
  WAIT_CS
};

ParseState parseState = WAIT_H0;
uint8_t parseCmd = 0;
uint8_t parseLen = 0;
uint8_t parseGot = 0;
uint8_t parsePayload[MAX_PAYLOAD];

void resetParser() {
  parseState = WAIT_H0;
  parseCmd = 0;
  parseLen = 0;
  parseGot = 0;
}

void resetSession() {
  resetParser();
  lastPingMs = 0;
  serialLine = "";
}

const char *cmdName(uint8_t cmd) {
  switch (cmd) {
    case CMD_PING:
      return "PING";
    case CMD_PONG:
      return "PONG";
    case CMD_SET_LED:
      return "SET_LED";
    case CMD_LED_ACK:
      return "LED_ACK";
    default:
      return "UNKNOWN";
  }
}

void logFrame(const char *dir, uint8_t cmd, const uint8_t *payload, uint8_t len) {
  Serial.print(dir);
  Serial.print(cmdName(cmd));
  if (cmd == CMD_SET_LED || cmd == CMD_LED_ACK) {
    if (len >= 1) {
      Serial.print(payload[0] ? " on" : " off");
    }
  }
  Serial.println();
}

bool sendFrame(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  if (!client.connected()) {
    return false;
  }
  if (len > MAX_PAYLOAD) {
    return false;
  }
  uint8_t cs = cmd ^ len;
  for (uint8_t i = 0; i < len; i++) {
    cs ^= payload[i];
  }
  uint8_t buf[5 + MAX_PAYLOAD];
  size_t n = 0;
  buf[n++] = HDR0;
  buf[n++] = HDR1;
  buf[n++] = cmd;
  buf[n++] = len;
  for (uint8_t i = 0; i < len; i++) {
    buf[n++] = payload[i];
  }
  buf[n++] = cs;
  client.write(buf, n);
  logFrame("tx: ", cmd, payload, len);
  return true;
}

void applyLed(uint8_t on) {
  digitalWrite(LED_PIN, on ? LED_ON : LED_OFF);
}

void handleFrame(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  switch (cmd) {
    case CMD_PING:
      logFrame("rx: ", cmd, payload, len);
      sendFrame(CMD_PONG, NULL, 0);
      break;
    case CMD_PONG:
      logFrame("rx: ", cmd, payload, len);
      break;
    case CMD_SET_LED:
      if (len != 1) {
        Serial.println("bad frame");
        return;
      }
      logFrame("rx: ", cmd, payload, len);
      applyLed(payload[0] ? 1 : 0);
      {
        uint8_t ack = payload[0] ? 1 : 0;
        sendFrame(CMD_LED_ACK, &ack, 1);
      }
      break;
    case CMD_LED_ACK:
      if (len != 1) {
        Serial.println("bad frame");
        return;
      }
      logFrame("rx: ", cmd, payload, len);
      break;
    default:
      Serial.println("bad frame");
      break;
  }
}

void feedByte(uint8_t b) {
  switch (parseState) {
    case WAIT_H0:
      if (b == HDR0) {
        parseState = WAIT_H1;
      }
      break;
    case WAIT_H1:
      if (b == HDR1) {
        parseState = WAIT_CMD;
      } else if (b == HDR0) {
        parseState = WAIT_H1;
      } else {
        parseState = WAIT_H0;
      }
      break;
    case WAIT_CMD:
      parseCmd = b;
      parseState = WAIT_LEN;
      break;
    case WAIT_LEN:
      parseLen = b;
      if (parseLen > MAX_PAYLOAD) {
        Serial.println("bad frame");
        resetParser();
      } else if (parseLen == 0) {
        parseState = WAIT_CS;
      } else {
        parseGot = 0;
        parseState = WAIT_PAYLOAD;
      }
      break;
    case WAIT_PAYLOAD:
      parsePayload[parseGot++] = b;
      if (parseGot >= parseLen) {
        parseState = WAIT_CS;
      }
      break;
    case WAIT_CS: {
      uint8_t cs = parseCmd ^ parseLen;
      for (uint8_t i = 0; i < parseLen; i++) {
        cs ^= parsePayload[i];
      }
      if (cs != b) {
        Serial.println("bad frame");
      } else {
        handleFrame(parseCmd, parsePayload, parseLen);
      }
      resetParser();
      break;
    }
  }
}

void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      serialLine.trim();
      if (serialLine.length() > 0) {
        if (serialLine == "on") {
          uint8_t on = 1;
          sendFrame(CMD_SET_LED, &on, 1);
        } else if (serialLine == "off") {
          uint8_t on = 0;
          sendFrame(CMD_SET_LED, &on, 1);
        } else {
          Serial.println("unknown cmd");
        }
      }
      serialLine = "";
    } else {
      serialLine += c;
      if (serialLine.length() > SERIAL_LINE_MAX) {
        serialLine = "";
      }
    }
  }
}

void setup() {
  Serial.begin(BAUD);
  delay(200);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

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
    feedByte((uint8_t)client.read());
  }

  handleSerial();

  now = millis();
  if (now - lastPingMs >= SEND_INTERVAL_MS) {
    lastPingMs = now;
    sendFrame(CMD_PING, NULL, 0);
  }
}
