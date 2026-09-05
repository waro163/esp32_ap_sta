#include <WiFi.h>

#define WIFI_SSID "ESP32_LINK"
#define WIFI_PASS "esp32link"
#define TCP_PORT 8080
#define BAUD 115200
#define LED_PIN 2
#define LED_ON HIGH
#define LED_OFF LOW

#define HDR0 0xAA
#define HDR1 0x55
#define CMD_PING 0x01
#define CMD_PONG 0x02
#define CMD_SET_LED 0x03
#define CMD_LED_ACK 0x04
#define CMD_SET_THROTTLE 0x05
#define MAX_PAYLOAD 32

#define ADC_PIN 32
#define WINDOW_SIZE 10
#define THRESHOLD 50
#define CALIBRATE_DISCARD 20
#define CALIBRATE_SAMPLES 32
#define MIN_TRAVEL 200
#define ADC_FULL_SCALE 4095
#define DUTY_MIN 205
#define DUTY_MAX 410
#define ADC_POLL_MS 10

WiFiServer server(TCP_PORT);
WiFiClient client;

bool hadClient = false;

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

int adcBuffer[WINDOW_SIZE] = {0};
int bufferIndex = 0;
int adcSum = 0;
int middleValue = 0;
float dutyUnit = 0;
int lastDuty = DUTY_MIN;
int lastSentDuty = -1;
bool forceSendThrottle = false;
uint32_t lastAdcPollMs = 0;

void resetParser() {
  parseState = WAIT_H0;
  parseCmd = 0;
  parseLen = 0;
  parseGot = 0;
}

void resetSession() {
  resetParser();
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
    case CMD_SET_THROTTLE:
      return "THROTTLE";
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
  } else if (cmd == CMD_SET_THROTTLE) {
    if (len >= 2) {
      uint16_t duty = ((uint16_t)payload[0] << 8) | payload[1];
      Serial.print(' ');
      Serial.print(duty);
    }
  }
  Serial.println();
}

bool sendFrame(uint8_t cmd, const uint8_t *payload, uint8_t len) {
  if (!client || !client.connected()) {
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

void calibrateAdc() {
  analogSetPinAttenuation(ADC_PIN, ADC_11db);

  for (int i = 0; i < CALIBRATE_DISCARD; i++) {
    analogRead(ADC_PIN);
    delay(2);
  }

  long calibSum = 0;
  for (int i = 0; i < CALIBRATE_SAMPLES; i++) {
    calibSum += analogRead(ADC_PIN);
    delay(2);
  }
  int initialVal = (int)(calibSum / CALIBRATE_SAMPLES);

  for (int i = 0; i < WINDOW_SIZE; i++) {
    adcBuffer[i] = initialVal;
  }
  adcSum = initialVal * WINDOW_SIZE;
  bufferIndex = 0;
  middleValue = initialVal;

  int travel = ADC_FULL_SCALE - middleValue;
  if (travel < MIN_TRAVEL) {
    Serial.println("Warning: ADC near full scale at boot, throttle mapping limited.");
    travel = MIN_TRAVEL;
  }
  dutyUnit = (float)(DUTY_MAX - DUTY_MIN) / travel;

  Serial.print("System Initialized. Center Value: ");
  Serial.println(middleValue);
}

int readFilteredAdc() {
  int current = analogRead(ADC_PIN);
  adcSum = adcSum - adcBuffer[bufferIndex] + current;
  adcBuffer[bufferIndex] = current;
  bufferIndex++;
  if (bufferIndex >= WINDOW_SIZE) {
    bufferIndex = 0;
  }
  return adcSum / WINDOW_SIZE;
}

int adcToDuty(int adc) {
  int midDiff = adc - middleValue;
  if (midDiff < THRESHOLD) {
    return DUTY_MIN;
  }
  int dutyNow = DUTY_MIN + (int)(midDiff * dutyUnit + 0.5f);
  if (dutyNow > DUTY_MAX) {
    dutyNow = DUTY_MAX;
  }
  return dutyNow;
}

bool sendThrottle(uint16_t duty) {
  uint8_t payload[2];
  payload[0] = (uint8_t)(duty >> 8);
  payload[1] = (uint8_t)(duty & 0xFF);
  return sendFrame(CMD_SET_THROTTLE, payload, 2);
}

void pollThrottle() {
  uint32_t now = millis();
  if (now - lastAdcPollMs < ADC_POLL_MS) {
    return;
  }
  lastAdcPollMs = now;

  lastDuty = adcToDuty(readFilteredAdc());

  if (!client || !client.connected()) {
    return;
  }
  if (!forceSendThrottle && lastDuty == lastSentDuty) {
    return;
  }
  if (sendThrottle((uint16_t)lastDuty)) {
    lastSentDuty = lastDuty;
    forceSendThrottle = false;
  }
}

void setup() {
  Serial.begin(BAUD);
  delay(200);

  calibrateAdc();

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

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
  pollThrottle();

  if (!client || !client.connected()) {
    if (hadClient) {
      client.stop();
      hadClient = false;
      resetSession();
      Serial.println("waiting for STA...");
    }

    WiFiClient incoming = server.available();
    if (incoming && incoming.connected()) {
      client = incoming;
      client.setNoDelay(true);
      hadClient = true;
      resetSession();
      Serial.println("client matched");
      forceSendThrottle = true;
      lastSentDuty = -1;
    } else {
      return;
    }
  }

  while (client.available()) {
    feedByte((uint8_t)client.read());
  }
}
