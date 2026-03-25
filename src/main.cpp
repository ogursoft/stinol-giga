#include <Arduino.h>
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <NeoPixelBus.h>

// === Пины ===
#define LED_PIN     2
#define TEMP_PIN    4
#define DOOR_PIN    5
#define BUTTON_PIN  0
#define RELAY_PIN   12

// === NeoPixel ===
#define NUM_LEDS 1
NeoPixelBus<NeoGrbFeature, NeoEsp8266Uart800KbpsMethod> strip(NUM_LEDS, LED_PIN);

// === Датчик температуры ===
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);

// === WiFi и MQTT ===
AsyncMqttClient mqttClient;
AsyncWebServer server(80);

// === Состояние ===
float currentTemp = 0;
bool compressorOn = false;
bool doorOpen = false;
unsigned long lastTempRead = 0;

// === Настройки ===
struct {
  char ssid[32];
  char password[64];
  char mqttBroker[64];
  int mqttPort;
  char mqttClientId[32];
  char mqttUser[32];
  char mqttPass[32];
  char topicPrefix[32];
  float tempTarget;
  float hysteresis;
  int ledBrightness;
} settings;

// === Защита компрессора ===
unsigned long lastStopTime = 0;
unsigned long startTime = 0;

bool canStartCompressor() {
  if (compressorOn) return true;
  return millis() - lastStopTime >= 5 * 60 * 1000UL; // 5 мин пауза
}

void startCompressor() {
  if (!compressorOn && canStartCompressor()) {
    digitalWrite(RELAY_PIN, HIGH);
    compressorOn = true;
    startTime = millis();
  }
}

void stopCompressor() {
  digitalWrite(RELAY_PIN, LOW);
  compressorOn = false;
  lastStopTime = millis();
}

// === Загрузка настроек ===
bool loadSettings() {
  if (!LittleFS.begin()) return false;
  File file = LittleFS.open("/settings.json", "r");
  if (!file) return false;

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) return false;

  strlcpy(settings.ssid, doc["wifi"]["ssid"], 32);
  strlcpy(settings.password, doc["wifi"]["password"], 64);
  strlcpy(settings.mqttBroker, doc["mqtt"]["broker"], 64);
  settings.mqttPort = doc["mqtt"]["port"] | 1883;
  strlcpy(settings.mqttClientId, doc["mqtt"]["client_id"], 32);
  strlcpy(settings.mqttUser, doc["mqtt"]["username"], 32);
  strlcpy(settings.mqttPass, doc["mqtt"]["password"], 32);
  strlcpy(settings.topicPrefix, doc["mqtt"]["topic_prefix"], 32);
  settings.tempTarget = doc["temp_target"] | -18.0;
  settings.hysteresis = doc["hysteresis"] | 1.0;
  settings.ledBrightness = doc["led_brightness"] | 50;

  return true;
}

void saveSettings() {
  DynamicJsonDocument doc(2048);
  doc["wifi"]["ssid"] = settings.ssid;
  doc["wifi"]["password"] = settings.password;
  doc["mqtt"]["broker"] = settings.mqttBroker;
  doc["mqtt"]["port"] = settings.mqttPort;
  doc["mqtt"]["client_id"] = settings.mqttClientId;
  doc["mqtt"]["username"] = settings.mqttUser;
  doc["mqtt"]["password"] = settings.mqttPass;
  doc["mqtt"]["topic_prefix"] = settings.topicPrefix;
  doc["temp_target"] = settings.tempTarget;
  doc["hysteresis"] = settings.hysteresis;
  doc["led_brightness"] = settings.ledBrightness;

  File file = LittleFS.open("/settings.json", "w");
  serializeJson(doc, file);
  file.close();
}

// === RGB LED ===
void setLedColor(uint8_t r, uint8_t g, uint8_t b) {
  RgbColor color(r, g, b);
  color.Darken(255 - (settings.ledBrightness * 255 / 100));
  strip.SetPixelColor(0, color);
  strip.Show();
}

void blinkColor(uint8_t r, uint8_t g, uint8_t b, int delayMs = 500) {
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > delayMs) {
    bool on = (millis() / delayMs) % 2;
    setLedColor(on ? r : 0, on ? g : 0, on ? b : 0);
    lastBlink = millis();
  }
}

void updateLed() {
  if (!WiFi.isConnected()) {
    blinkColor(255, 165, 0); // оранжевый
  } else if (!mqttClient.connected()) {
    blinkColor(0, 0, 255); // синий
  } else if (compressorOn) {
    setLedColor(0, 0, 255); // синий
  } else if (doorOpen) {
    blinkColor(255, 255, 0); // жёлтый
  } else {
    setLedColor(0, 255, 0); // зелёный
  }
}

// === MQTT ===
void connectToMqtt();
void onMqttConnect(bool sessionPresent) {
  char subTopic[128];
  snprintf(subTopic, sizeof(subTopic), "%s/set_temp", settings.topicPrefix);
  mqttClient.subscribe(subTopic, 0);
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties,
                   size_t len, size_t index, size_t total) {
  String message;
  message.concat(payload, len);
  if (String(topic).endsWith("/set_temp")) {
    settings.tempTarget = message.toFloat();
    saveSettings();
  }
}

void setupMqtt() {
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onMessage(onMqttMessage);
  mqttClient.onDisconnect([](AsyncMqttClientDisconnectReason reason) { updateLed(); });
  mqttClient.setServer(settings.mqttBroker, settings.mqttPort);
  if (strlen(settings.mqttUser) > 0) {
    mqttClient.setCredentials(settings.mqttUser, settings.mqttPass);
  }
  mqttClient.setClientId(settings.mqttClientId);
  mqttClient.connect();
}

void publishStatus() {
  StaticJsonDocument<256> doc;
  doc["temp"] = currentTemp;
  doc["target"] = settings.tempTarget;
  doc["compressor"] = compressorOn;
  doc["door"] = doorOpen;
  doc["rssi"] = WiFi.RSSI();

  char jsonBuffer[256];
  serializeJson(doc, jsonBuffer);

  char topic[128];
  snprintf(topic, sizeof(topic), "%s/status", settings.topicPrefix);
  mqttClient.publish(topic, 0, false, jsonBuffer);
}

// === Веб-сервер ===
void initWebServer() {
  #include <AsyncElegantOTA.h>
  AsyncElegantOTA.begin(&server);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(128);
    doc["temp"] = currentTemp;
    doc["target"] = settings.tempTarget;
    doc["compressor"] = compressorOn;
    doc["door"] = doorOpen;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(256);
    doc["temp_target"] = settings.tempTarget;
    doc["hysteresis"] = settings.hysteresis;
    doc["led_brightness"] = settings.ledBrightness;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest *request) {}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, data);
    if (!error) {
      settings.tempTarget = doc["temp_target"];
      settings.hysteresis = doc["hysteresis"];
      settings.ledBrightness = doc["led_brightness"];
      saveSettings();
    }
    request->send(200, "text/plain", "OK");
  });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.begin();
}

// === Кнопка ===
void IRAM_ATTR handleButton() {
  static unsigned long last = 0;
  if (millis() - last > 200) {
    if (!digitalRead(BUTTON_PIN)) {
      if (compressorOn) stopCompressor();
      else if (canStartCompressor()) startCompressor();
    }
    last = millis();
  }
}

void setup() {
  Serial.begin(115200);
  strip.Begin();
  setLedColor(255, 255, 255); // белый при старте

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(DOOR_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButton, FALLING);

  if (!loadSettings()) {
    strcpy(settings.ssid, "your_wifi");
    strcpy(settings.password, "your_pass");
    strcpy(settings.mqttBroker, "192.168.1.100");
    settings.mqttPort = 1883;
    strcpy(settings.mqttClientId, "stinol_giga");
    settings.tempTarget = -18;
    settings.hysteresis = 1;
    settings.ledBrightness = 50;
  }

  WiFi.begin(settings.ssid, settings.password);
  sensors.begin();

  setupMqtt();
  initWebServer();
}

void loop() {
  doorOpen = !digitalRead(DOOR_PIN);

  if (millis() - lastTempRead > 5000) {
    sensors.requestTemperatures();
    currentTemp = sensors.getTempCByIndex(0);
    lastTempRead = millis();

    if (currentTemp > settings.tempTarget + settings.hysteresis) {
      if (canStartCompressor()) startCompressor();
    } else if (currentTemp < settings.tempTarget - settings.hysteresis) {
      stopCompressor();
    }

    publishStatus();
  }

  updateLed();
}
