#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <time.h>
#if __has_include(<driver/i2s_std.h>)
#include <driver/i2s_std.h>
#define P1S_LED_HAS_I2S_STD 1
#else
#include <driver/i2s.h>
#define P1S_LED_HAS_I2S_STD 0
#endif
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#define DEFAULT_WIFI_PASSWORD ""
#define DEFAULT_P1S_SERIAL ""
#define DEFAULT_BAMBU_CLOUD_USER ""
#define DEFAULT_BAMBU_CLOUD_TOKEN ""
#endif

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

namespace {
constexpr uint8_t LED_PIN = 4;
constexpr uint16_t LED_COUNT = 81;
constexpr uint8_t MIC_WS_PIN = 6;
constexpr uint8_t MIC_SCK_PIN = 5;
constexpr uint8_t MIC_SD_PIN = 7;
constexpr uint8_t DEFAULT_BRIGHTNESS = 100;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr uint32_t MQTT_RETRY_MS = 5000;
constexpr uint32_t STATUS_STALE_MS = 90000;
constexpr uint32_t FRAME_MS = 20;

CRGB leds[LED_COUNT];
WebServer server(80);
WiFiClientSecure tlsClient;
PubSubClient mqtt(tlsClient);
Preferences preferences;

enum class LedMode : uint8_t { STARTUP, STANDBY, HEATING, PRINTING, FINISHED, FILAMENT_LOADING, OFFLINE, PREVIEW };

struct Config {
  String wifiSsid;
  String wifiPassword;
  String printerSerial;
  String cloudUser;
  String cloudToken;
  uint8_t brightness = DEFAULT_BRIGHTNESS;
  uint16_t finishedSeconds = 300; // Changed default finish animation to 5 minutes (300 seconds)
};

struct PrinterStatus {
  String gcodeState = "UNKNOWN";
  float nozzle = NAN, nozzleTarget = NAN, bed = NAN, bedTarget = NAN;
  int progress = -1;
  int stage = -1;
  int subStage = -1;
  bool isFilamentLoading = false;
  bool isFilamentUnloading = false;
  uint32_t lastReportMs = 0;
  bool stateInitialized = false;
  bool isFirstReport = false;
  bool wasPrintingBeforeFinish = false; // Tracks if a print was running
};

Config config;
PrinterStatus printer;
LedMode mode = LedMode::STARTUP;
LedMode previewMode = LedMode::STANDBY;
uint32_t modeSinceMs = 0;
uint32_t finishedSinceMs = 0;
uint32_t finishLockUntilMs = 0; // Lockout timer for LedMode::FINISHED
uint32_t lastFrameMs = 0;
uint32_t lastMqttAttemptMs = 0;
float micAmplitude = 0;
#if P1S_LED_HAS_I2S_STD
i2s_chan_handle_t micRxHandle = nullptr;
#endif
uint8_t micReadErrors = 0;
bool setupApActive = false;
bool ntpRequested = false;
String eventLog[12];
uint8_t eventHead = 0;

// --- STANDBY ANIMATION STATE ---
const uint32_t STANDBY_BRIGHTNESS     = 100;
const uint32_t STANDBY_COLOR_INTERVAL  = 120000; // Changed to 2 Minutes
const uint32_t STANDBY_GLITCH_INTERVAL = 30000;  // Changed to 30 Seconds
const uint32_t STANDBY_TIMEOUT_MS       = 900000; // 15 Minutes standby timeout
const uint32_t STANDBY_FADE_MS          = 5000;   // 5 second fade out duration
const uint32_t STANDBY_PHASE_MS        = 2000;   // 2 Seconds
const uint16_t STANDBY_FLICKER_MS      = 200;    // 200ms flicker

CRGB standbyCurrentColor;
CRGB standbyTargetColor;
bool standbyInitialized = false;
uint32_t standbyLastColorChangeMs = 0;
uint32_t standbyLastGlitchTriggerMs = 0;

// --- FILAMENT LOADING STATE ---
uint16_t filamentHeadPos = 0;

enum GlitchPhase {
  GLITCH_IDLE,
  GLITCH_FLICKER_BASE,    // 2s flickering base color
  GLITCH_SOLID_OFF,       // 2s turned off
  GLITCH_SOLID_RANDOM,    // 2s solid distinct random color
  GLITCH_FLICKER_RANDOM   // 2s flickering random color
};

struct GlitchPixel {
  bool active = false;
  uint16_t index = 0;
  GlitchPhase phase = GLITCH_IDLE;
  uint32_t phaseStartMs = 0;
  uint32_t lastFlickerMs = 0;
  bool flickerToggle = false;
  CRGB glitchColor = CRGB::Black;
};

GlitchPixel activeGlitches[3];

const char *modeName(LedMode value) {
  switch (value) {
    case LedMode::STARTUP: return "startup";
    case LedMode::STANDBY: return "standby";
    case LedMode::HEATING: return "heating";
    case LedMode::PRINTING: return "printing";
    case LedMode::FINISHED: return "finished";
    case LedMode::FILAMENT_LOADING: return "filament_loading";
    case LedMode::OFFLINE: return "offline";
    case LedMode::PREVIEW: return "preview";
  }
  return "unknown";
}

void addEvent(const String &message) {
  eventLog[eventHead] = String(millis() / 1000) + "s: " + message;
  eventHead = (eventHead + 1) % 12;
  Serial.println(message);
}

CRGB getRandomColor() {
  return CHSV(random8(), 240, 255);
}

CRGB getDistinctRandomColor(CRGB base) {
  CRGB newColor;
  do {
    newColor = getRandomColor();
  } while (newColor == base);
  return newColor;
}

// Sends MQTT payload to Bambu Printer to turn off the hardware light bar
void turnOffPrinterLightBar() {
  if (!mqtt.connected() || !config.printerSerial.length()) return;
  String requestTopic = "device/" + config.printerSerial + "/request";
  String payload = "{\"system\":{\"sequence_id\":\"0\",\"command\":\"ledctrl\",\"led_node\":\"chamber_light\",\"led_mode\":\"off\",\"led_on_time\":0,\"led_off_time\":0,\"loop_times\":0,\"interval_time\":0}}";
  mqtt.publish(requestTopic.c_str(), payload.c_str());
  addEvent("Command sent: Chamber light bar OFF");
}

void initStandbyColors(bool forceNewColor = false) {
  if (!standbyInitialized || forceNewColor) {
    standbyCurrentColor = getRandomColor();
    standbyTargetColor  = standbyCurrentColor;
    standbyInitialized  = true;
  }
  standbyLastColorChangeMs = millis();
  standbyLastGlitchTriggerMs = millis();
  for (uint8_t i = 0; i < 3; i++) activeGlitches[i].active = false;
}

void loadConfig() {
  preferences.begin("p1sled", false);
  config.wifiSsid = preferences.getString("wifi_ssid", "");
  config.wifiPassword = preferences.getString("wifi_pass", "");
  config.printerSerial = preferences.getString("p_serial", "");
  config.cloudUser = preferences.getString("cloud_user", "");
  config.cloudToken = preferences.getString("cloud_token", "");
  config.brightness = preferences.getUChar("bright", DEFAULT_BRIGHTNESS);
  config.finishedSeconds = preferences.getUShort("finish_sec", 300); // 5 Minutes
  preferences.end();

  if (!config.wifiSsid.length()) config.wifiSsid = DEFAULT_WIFI_SSID;
  if (!config.wifiPassword.length()) config.wifiPassword = DEFAULT_WIFI_PASSWORD;
  if (!config.printerSerial.length()) config.printerSerial = DEFAULT_P1S_SERIAL;
  if (!config.cloudUser.length()) config.cloudUser = DEFAULT_BAMBU_CLOUD_USER;
  if (!config.cloudToken.length()) config.cloudToken = DEFAULT_BAMBU_CLOUD_TOKEN;
}

void saveConfig() {
  preferences.begin("p1sled", false);
  preferences.putString("wifi_ssid", config.wifiSsid);
  preferences.putString("wifi_pass", config.wifiPassword);
  preferences.putString("p_serial", config.printerSerial);
  preferences.putString("cloud_user", config.cloudUser);
  preferences.putString("cloud_token", config.cloudToken);
  preferences.putUChar("bright", config.brightness);
  preferences.putUShort("finish_sec", config.finishedSeconds);
  preferences.end();
}

String mqttConfigurationIssue() {
  if (!config.printerSerial.length()) return "printer serial is missing";
  if (!config.cloudUser.length()) return "Bambu Cloud user ID is missing";
  if (!config.cloudToken.length()) return "Bambu Cloud access token is missing";
  return "";
}

bool configuredForMqtt() {
  return !mqttConfigurationIssue().length();
}

void startSetupAp() {
  if (setupApActive) return;
  uint32_t chip = static_cast<uint32_t>(ESP.getEfuseMac());
  String suffix = String(chip & 0xFFFF, HEX);
  suffix.toUpperCase();
  String ssid = "P1S-LED-Setup-" + suffix;
  String password = "configure-" + suffix;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid.c_str(), password.c_str());
  setupApActive = true;
  addEvent("Setup AP " + ssid + " started; password: " + password);
}

bool connectWifi() {
  if (!config.wifiSsid.length()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    addEvent("Wi-Fi connected: " + WiFi.localIP().toString());
    configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
    ntpRequested = true;
    addEvent("Synchronizing clock for TLS certificate verification");
    return true;
  }
  addEvent("Wi-Fi join timed out");
  return false;
}

bool hasValidClock() {
  return time(nullptr) >= 1735689600; // 2025-01-01 UTC
}

void setupMicrophone() {
#if P1S_LED_HAS_I2S_STD
  i2s_chan_config_t channelConfig = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_std_config_t standardConfig = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = static_cast<gpio_num_t>(MIC_SCK_PIN),
      .ws = static_cast<gpio_num_t>(MIC_WS_PIN),
      .dout = I2S_GPIO_UNUSED,
      .din = static_cast<gpio_num_t>(MIC_SD_PIN),
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_new_channel(&channelConfig, nullptr, &micRxHandle) == ESP_OK &&
      i2s_channel_init_std_mode(micRxHandle, &standardConfig) == ESP_OK &&
      i2s_channel_enable(micRxHandle) == ESP_OK) {
    addEvent("I2S microphone diagnostics enabled");
  } else {
    micRxHandle = nullptr;
    addEvent("I2S microphone initialization failed");
  }
#else
  i2s_config_t legacyConfig = {};
  legacyConfig.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  legacyConfig.sample_rate = 16000;
  legacyConfig.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  legacyConfig.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  legacyConfig.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  legacyConfig.dma_buf_count = 4;
  legacyConfig.dma_buf_len = 64;
  i2s_pin_config_t pins = {};
  pins.bck_io_num = MIC_SCK_PIN;
  pins.ws_io_num = MIC_WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_SD_PIN;
  if (i2s_driver_install(I2S_NUM_0, &legacyConfig, 0, nullptr) == ESP_OK &&
      i2s_set_pin(I2S_NUM_0, &pins) == ESP_OK) {
    addEvent("Legacy I2S microphone diagnostics enabled");
  } else {
    addEvent("Legacy I2S microphone initialization failed");
  }
#endif
}

void updateMicAmplitude() {
#if P1S_LED_HAS_I2S_STD
  if (!micRxHandle) return;
  int32_t samples[32];
  size_t received = 0;
  if (i2s_channel_read(micRxHandle, samples, sizeof(samples), &received, pdMS_TO_TICKS(10)) != ESP_OK || !received) {
    if (++micReadErrors > 5) {
      i2s_channel_disable(micRxHandle);
      delay(10);
      i2s_channel_enable(micRxHandle);
      micReadErrors = 0;
    }
    return;
  }
  micReadErrors = 0;
#else
  int32_t samples[32];
  size_t received = 0;
  if (i2s_read(I2S_NUM_0, samples, sizeof(samples), &received, 0) != ESP_OK || !received) return;
#endif
  double total = 0.0;
  size_t count = received / sizeof(int32_t);
  for (size_t i = 0; i < count; ++i) {
    double normalized = samples[i] / 2147483648.0;
    total += normalized * normalized;
  }
  float rms = sqrt(total / count);
  micAmplitude = micAmplitude * 0.85f + rms * 0.15f;
}

bool isPrintingState(const String &state) {
  return state == "RUNNING" || state == "PAUSE" || state == "PAUSED";
}
bool isFinishedState(const String &state) {
  return state == "FINISH" || state == "FINISHED" || state == "COMPLETED";
}
bool isHeating() {
  return (!isnan(printer.nozzleTarget) && !isnan(printer.nozzle) && printer.nozzleTarget > printer.nozzle + 4.0f) ||
         (!isnan(printer.bedTarget) && !isnan(printer.bed) && printer.bedTarget > printer.bed + 3.0f);
}

void setMode(LedMode newMode) {
  // If the 5-minute lock is active, reject all non-FINISHED state changes
  if (finishLockUntilMs > 0) {
    if ((int32_t)(millis() - finishLockUntilMs) < 0) {
      if (newMode != LedMode::FINISHED) {
        return; // Ignore mode changes while locked
      }
    } else {
      finishLockUntilMs = 0; // Lock duration has elapsed
    }
  }

  if (mode == newMode) return;
  mode = newMode;
  modeSinceMs = millis(); // Track when this mode started

  // Execute entry actions for new modes...
}

void updateModeFromPrinter() {
  if (mode == LedMode::PREVIEW) return;

  // 1. Check if the 5-minute lock was active and has just expired
  if (finishLockUntilMs > 0 && (int32_t)(millis() - finishLockUntilMs) >= 0) {
    finishLockUntilMs = 0; // Clear lock
    printer.wasPrintingBeforeFinish = false; // Reset session tracking
    setMode(LedMode::STANDBY); // Transition directly to Standby
    return;
  }

  // 2. Lock active: Maintain FINISHED mode and ignore all MQTT dropouts until timer is up
  if (finishLockUntilMs > 0) {
    setMode(LedMode::FINISHED);
    return;
  }

  // 3. Handle connection drops (Moved BELOW the finish lock checks)
  if (WiFi.status() != WL_CONNECTED || !mqtt.connected() ||
      (printer.lastReportMs && millis() - printer.lastReportMs > STATUS_STALE_MS)) {
    finishLockUntilMs = 0; 
    setMode(LedMode::OFFLINE);
    return;
  }

  // Track if printer enters a running/printing state
  if (isPrintingState(printer.gcodeState)) {
    printer.wasPrintingBeforeFinish = true;
    setMode(LedMode::PRINTING);
    return;
  }

  // Lock inactive: Detect FINISH event from printer
  if (isFinishedState(printer.gcodeState)) {
    // Only trigger finish animation if it was actually printing first during this session
    if (printer.wasPrintingBeforeFinish && !printer.isFirstReport) {
      // Set the lockout timer exactly once when finish is first detected
      if (finishLockUntilMs == 0) {
        finishLockUntilMs = millis() + (static_cast<uint32_t>(config.finishedSeconds) * 1000UL);
        turnOffPrinterLightBar();
      }
      setMode(LedMode::FINISHED);
      return;
    } else {
      // If it booted up already finished or skipped printing, go straight to standby
      setMode(LedMode::STANDBY);
      return;
    }
  }

  printer.isFirstReport = false;

  // Standard operational modes
  if (isHeating() || printer.gcodeState == "PREPARE") setMode(LedMode::HEATING);
  else setMode(LedMode::STANDBY);
}

float readNumber(JsonVariantConst object, const char *key, float fallback) {
  JsonVariantConst value = object[key];
  return value.is<float>() || value.is<int>() ? value.as<float>() : fallback;
}

int readInt(JsonVariantConst value, int fallback = -1) {
  if (value.is<int>()) return value.as<int>();
  if (value.is<long>()) return static_cast<int>(value.as<long>());
  if (value.is<const char*>()) {
    const char* str = value.as<const char*>();
    if (str && str[0] != '\0') return atoi(str);
  }
  return fallback;
}

bool isUnloadStage(int stg) {
  return (stg == 5 || stg == 20 || stg == 21 || stg == 24 || stg == 25 || stg == 26 || stg == 27 || stg == 35 || stg == 37 || stg == 38);
}

bool isLoadStage(int stg) {
  return (stg == 4 || stg == 22 || stg == 23 || stg == 28 || stg == 29 || stg == 30 || stg == 31 || stg == 32 || stg == 33 || stg == 36);
}

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  if (length == 0 || length > 8192) { addEvent("Rejected malformed MQTT payload"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) { addEvent("Rejected invalid MQTT JSON"); return; }
  JsonVariantConst print = doc["print"];
  if (!print.is<JsonObjectConst>()) return;
  
  if (!printer.stateInitialized) {
    printer.isFirstReport = true;
    printer.stateInitialized = true;
  } else {
    printer.isFirstReport = false;
  }

  String oldState = printer.gcodeState;
  const char *receivedState = print["gcode_state"] | "";
  if (receivedState[0]) printer.gcodeState = receivedState;
  
  printer.nozzle = readNumber(print, "nozzle_temper", printer.nozzle);
  printer.nozzleTarget = readNumber(print, "nozzle_target_temper", printer.nozzleTarget);
  printer.bed = readNumber(print, "bed_temper", printer.bed);
  printer.bedTarget = readNumber(print, "bed_target_temper", printer.bedTarget);
  if (print["mc_percent"].is<int>()) printer.progress = print["mc_percent"].as<int>();

  int stgVal = readInt(print["stg_cur"]);
  if (stgVal == -1) stgVal = readInt(print["stg"]);
  if (stgVal == -1) stgVal = readInt(print["mc_print_stage"]);
  if (stgVal != -1) printer.stage = stgVal;

  int subStgVal = readInt(print["mc_print_sub_stage"]);
  if (subStgVal == -1) subStgVal = readInt(print["mc_stage"]);
  if (subStgVal != -1) printer.subStage = subStgVal;

  int amsStatus = readInt(print["ams_status"]);

  bool unloadDetected = isUnloadStage(printer.stage) || isUnloadStage(printer.subStage) || (amsStatus == 7);
  bool loadDetected   = isLoadStage(printer.stage)   || isLoadStage(printer.subStage)   || (amsStatus == 6);

  String mcStageStr = print["mc_stage"] | "";
  mcStageStr.toUpperCase();
  if (mcStageStr.indexOf("UNLOAD") != -1 || mcStageStr.indexOf("CUT") != -1 || mcStageStr.indexOf("RETRACT") != -1) {
    unloadDetected = true;
  } else if (mcStageStr.indexOf("LOAD") != -1 || mcStageStr.indexOf("PUSH") != -1 || mcStageStr.indexOf("INSERT") != -1) {
    loadDetected = true;
  }

  printer.isFilamentUnloading = unloadDetected;
  printer.isFilamentLoading   = unloadDetected || loadDetected;

  printer.lastReportMs = millis();
  if (oldState != printer.gcodeState) addEvent("Printer state: " + printer.gcodeState);
  updateModeFromPrinter();
}

void connectMqttIfNeeded() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected() || !configuredForMqtt()) return;
  if (!ntpRequested) {
    configTime(0, 0, "time.cloudflare.com", "pool.ntp.org");
    ntpRequested = true;
  }
  if (!hasValidClock()) return;
  if (millis() - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = millis();
  tlsClient.stop();
  tlsClient.setCACertBundle(rootca_crt_bundle_start);
  tlsClient.setTimeout(15);
  mqtt.setServer("us.mqtt.bambulab.com", 8883);
  mqtt.setBufferSize(8192);
  String clientId = "p1s-led-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
  String username = "u_" + config.cloudUser;
  
  if (mqtt.connect(clientId.c_str(), username.c_str(), config.cloudToken.c_str())) {
    String topic = "device/" + config.printerSerial + "/report";
    if (mqtt.subscribe(topic.c_str())) {
      addEvent("Subscribed to Bambu Cloud report topic");
      
      String requestTopic = "device/" + config.printerSerial + "/request";
      String payload = "{\"pushing\": {\"sequence_id\": \"1\", \"command\": \"pushall\"}}";
      mqtt.publish(requestTopic.c_str(), payload.c_str());
      
    } else { 
      addEvent("MQTT subscription failed"); 
      mqtt.disconnect(); 
    }
  } else {
    addEvent("MQTT authentication/subscription connection failed (state " + String(mqtt.state()) + ")");
  }
}

void playStartupAnimation() {
  const uint16_t travelSteps = 40;
  const uint16_t pauseGap = 10;
  const uint16_t totalSteps = (travelSteps * 2) + pauseGap;

  const uint8_t totalLoops = 10;
  const uint16_t startDelay = 10;
  const uint16_t endDelay = 5;

  const CRGB loopColor = CRGB::Green;

  initStandbyColors();
  CRGB standbyColor = standbyCurrentColor;

  for (uint8_t loopCount = 0; loopCount < totalLoops; loopCount++) {
    uint16_t frameDelayMs = map(loopCount, 0, totalLoops - 1, startDelay, endDelay);

    for (uint16_t step = 0; step < totalSteps; step++) {
      if (loopCount == totalLoops - 1 && step >= travelSteps) {
        break; 
      }

      FastLED.clear();

      int16_t head = 0;
      int16_t tail = 0;

      if (step < travelSteps) {
        head = step;
        tail = 0;
      } 
      else if (step < travelSteps * 2) {
        head = travelSteps;
        tail = step - travelSteps;
      } 
      else {
        FastLED.show();
        delay(frameDelayMs);
        continue;
      }

      CRGB activeColor = loopColor;
      if (loopCount == totalLoops - 1) {
        activeColor = standbyColor;
      }

      for (int16_t i = tail; i <= head; i++) {
        uint16_t indexA = (70 - i + LED_COUNT) % LED_COUNT;
        uint16_t indexB = (70 + i) % LED_COUNT;

        leds[indexA] = activeColor;
        leds[indexB] = activeColor;
      }

      FastLED.show();
      delay(frameDelayMs);
    }
  }
  fill_solid(leds, LED_COUNT, standbyColor);
  FastLED.show();
}

void triggerGlitchSequence() {
  uint8_t count = random8(1, 4);
  for (uint8_t i = 0; i < count; i++) {
    activeGlitches[i].active = true;
    activeGlitches[i].index = random16(LED_COUNT);
    activeGlitches[i].phase = GLITCH_FLICKER_BASE;
    activeGlitches[i].phaseStartMs = millis();
    activeGlitches[i].lastFlickerMs = millis();
    activeGlitches[i].flickerToggle = false;
    activeGlitches[i].glitchColor = getDistinctRandomColor(standbyCurrentColor);
  }
}

void renderStandby() {
  uint32_t now = millis();
  uint32_t standbyElapsed = now - modeSinceMs;

  // Turn off LEDs completely after 15-minute standby duration + 5-second fade
  if (standbyElapsed >= STANDBY_TIMEOUT_MS + STANDBY_FADE_MS) {
    FastLED.clear();
    return;
  }

  FastLED.setBrightness(STANDBY_BRIGHTNESS);

  if (now - standbyLastColorChangeMs >= STANDBY_COLOR_INTERVAL) {
    standbyLastColorChangeMs = now;
    standbyTargetColor = getDistinctRandomColor(standbyCurrentColor);
  }

  EVERY_N_MILLISECONDS(50) {
    nblend(standbyCurrentColor, standbyTargetColor, 4);
  }

  fill_solid(leds, LED_COUNT, standbyCurrentColor);

  if (now - standbyLastGlitchTriggerMs >= STANDBY_GLITCH_INTERVAL) {
    standbyLastGlitchTriggerMs = now;
    triggerGlitchSequence();
  }

  for (uint8_t i = 0; i < 3; i++) {
    if (!activeGlitches[i].active) continue;

    if (now - activeGlitches[i].phaseStartMs >= STANDBY_PHASE_MS) {
      activeGlitches[i].phaseStartMs = now;

      switch (activeGlitches[i].phase) {
        case GLITCH_FLICKER_BASE:
          activeGlitches[i].phase = GLITCH_SOLID_OFF;
          break;
        case GLITCH_SOLID_OFF:
          activeGlitches[i].phase = GLITCH_SOLID_RANDOM;
          break;
        case GLITCH_SOLID_RANDOM:
          activeGlitches[i].phase = GLITCH_FLICKER_RANDOM;
          break;
        case GLITCH_FLICKER_RANDOM:
          activeGlitches[i].active = false;
          activeGlitches[i].phase = GLITCH_IDLE;
          continue;
        default:
          break;
      }
    }

    if (now - activeGlitches[i].lastFlickerMs >= STANDBY_FLICKER_MS) {
      activeGlitches[i].lastFlickerMs = now;
      activeGlitches[i].flickerToggle = !activeGlitches[i].flickerToggle;
    }

    uint16_t idx = activeGlitches[i].index;
    switch (activeGlitches[i].phase) {
      case GLITCH_FLICKER_BASE:
        leds[idx] = activeGlitches[i].flickerToggle ? standbyCurrentColor : CRGB::Black;
        break;
      case GLITCH_SOLID_OFF:
        leds[idx] = CRGB::Black;
        break;
      case GLITCH_SOLID_RANDOM:
        leds[idx] = activeGlitches[i].glitchColor;
        break;
      case GLITCH_FLICKER_RANDOM:
        leds[idx] = activeGlitches[i].flickerToggle ? activeGlitches[i].glitchColor : CRGB::Black;
        break;
      default:
        break;
    }
  }

  // Smooth fade-out animation after 15 minutes of standby
  if (standbyElapsed > STANDBY_TIMEOUT_MS) {
    uint32_t fadeProgress = standbyElapsed - STANDBY_TIMEOUT_MS;
    uint8_t fadeAmount = map(fadeProgress, 0, STANDBY_FADE_MS, 0, 255);
    fadeToBlackBy(leds, LED_COUNT, fadeAmount);
  }
}

void renderFinishedAnimation() {
  uint8_t currentBrightness = beatsin8(14, 0, 100);
  fill_solid(leds, LED_COUNT, CRGB::Green);
  FastLED.setBrightness(currentBrightness);
}

void renderFilamentLoadingAnimation() {
  FastLED.setBrightness(config.brightness);

  const uint8_t TAIL_FADE_AMOUNT = 60;
  const CRGB COMET_COLOR = CRGB(255, 180, 0);

  fadeToBlackBy(leds, LED_COUNT, TAIL_FADE_AMOUNT);

  uint16_t step = (millis() / 20) % LED_COUNT;
  uint16_t headPos = printer.isFilamentUnloading ? (LED_COUNT - 1 - step) : step;
  uint16_t oppositeHeadPos = (headPos + (LED_COUNT / 2)) % LED_COUNT;

  leds[headPos] = COMET_COLOR;
  leds[oppositeHeadPos] = COMET_COLOR;
}

void renderLeds() {
  if (millis() - lastFrameMs < FRAME_MS) return;
  lastFrameMs = millis();
  LedMode shown = mode == LedMode::PREVIEW ? previewMode : mode;
  
  switch (shown) {
    case LedMode::STARTUP:
      playStartupAnimation();
      break;
    case LedMode::STANDBY:
      renderStandby();
      break;
    case LedMode::HEATING:
      FastLED.setBrightness(config.brightness);
      fill_solid(leds, LED_COUNT, CRGB::Orange);
      break;
    case LedMode::FILAMENT_LOADING:
      renderFilamentLoadingAnimation();
      break;
    case LedMode::PRINTING:
      FastLED.setBrightness(config.brightness);
      fill_solid(leds, LED_COUNT, CRGB::White);
      break;
    case LedMode::FINISHED:
      renderFinishedAnimation();
      break;
    case LedMode::OFFLINE:
      // Non-blocking offline indicator replaces playStartupAnimation()
      
      break;
    case LedMode::PREVIEW: 
      break;
  }
  FastLED.show();
}

String cloudDashboardHtml() {
  return R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><style>body{font-family:system-ui;max-width:720px;margin:auto;padding:16px;background:#10131a;color:#e8edf5}input,button{width:100%;box-sizing:border-box;margin:5px 0;padding:9px}button{background:#216bd7;color:white;border:0;border-radius:4px}.grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}@media(max-width:500px){.grid{grid-template-columns:1fr}}</style></head><body><h1>P1S LED Controller</h1><div id="status">Loading…</div><p><button onclick="preview('standby')">Preview standby</button><button onclick="preview('heating')">Preview heating</button><button onclick="preview('loading')">Preview filament loading</button><button onclick="preview('unloading')">Preview filament unloading</button><button onclick="preview('printing')">Preview printing</button><button onclick="preview('finished')">Preview finished</button><button onclick="preview('auto')">Return to automatic</button></p><form method="post" action="/api/settings"><fieldset><legend>Wi-Fi Configuration</legend><input name="wifiSsid" id="wifiSsid" placeholder="Wi-Fi SSID"><input name="wifiPassword" id="wifiPassword" type="password" placeholder="Wi-Fi Password (leave blank to keep current)"></fieldset><fieldset><legend>Bambu Cloud MQTT — Americas</legend><input name="printerSerial" id="printerSerial" placeholder="Printer serial number"><input name="cloudUser" id="cloudUser" placeholder="Bambu Cloud user ID"><input name="cloudToken" id="cloudToken" type="password" placeholder="Bambu Cloud access token (leave blank to keep current)"><small>Uses Bambu Cloud; no P1S LAN address, LAN code, or local certificate is needed.</small></fieldset><fieldset><legend>LED behavior</legend><div class="grid"><label>Brightness (1–255)<input name="brightness" id="brightness" type="number" min="1" max="255"></label><label>Finished duration, seconds<input name="finishedSeconds" id="finishedSeconds" type="number" min="10" max="3600"></label></div></fieldset><button>Save and restart</button></form><script>let inputsFilled=false;async function refresh(){let d=await (await fetch('/api/status')).json();document.querySelector('#status').innerHTML='<h2>'+d.mode+'</h2><p>Wi-Fi: '+d.wifi+' · Cloud MQTT: '+d.mqtt+'<br>Clock: '+(d.clockSynchronized?'ready':'syncing')+' · Setup: '+(d.mqttConfiguration||'ready')+'<br>Printer: '+d.printerState+' · Stage: '+d.stage+' · '+d.progress+'%<br>Nozzle: '+d.nozzle+' / '+d.nozzleTarget+' °C · Bed: '+d.bed+' / '+d.bedTarget+' °C</p><pre>'+d.events.join('\n')+'</pre>';if(!inputsFilled){if(d.wifiSsid)document.querySelector('#wifiSsid').value=d.wifiSsid;if(d.printerSerial)document.querySelector('#printerSerial').value=d.printerSerial;if(d.cloudUser)document.querySelector('#cloudUser').value=d.cloudUser;if(d.brightness)document.querySelector('#brightness').value=d.brightness;if(d.finishedSeconds)document.querySelector('#finishedSeconds').value=d.finishedSeconds;inputsFilled=true}}async function preview(m){await fetch('/api/preview?mode='+m,{method:'POST'});refresh()}refresh();setInterval(refresh,2000)</script></body></html>)HTML";
}

void handleStatus() {
  JsonDocument doc;
  doc["mode"] = modeName(mode == LedMode::PREVIEW ? previewMode : mode);
  doc["automatic"] = mode != LedMode::PREVIEW;
  doc["wifi"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "disconnected";
  doc["wifiSsid"] = config.wifiSsid;
  doc["printerSerial"] = config.printerSerial;
  doc["cloudUser"] = config.cloudUser;
  doc["brightness"] = config.brightness;
  doc["finishedSeconds"] = config.finishedSeconds;
  doc["mqtt"] = mqtt.connected();
  doc["mqttConfiguration"] = mqttConfigurationIssue();
  doc["clockSynchronized"] = hasValidClock();
  doc["printerState"] = printer.gcodeState;
  doc["stage"] = printer.stage;
  doc["progress"] = printer.progress;
  if (isnan(printer.nozzle)) doc["nozzle"].set(nullptr); else doc["nozzle"] = printer.nozzle;
  if (isnan(printer.nozzleTarget)) doc["nozzleTarget"].set(nullptr); else doc["nozzleTarget"] = printer.nozzleTarget;
  if (isnan(printer.bed)) doc["bed"].set(nullptr); else doc["bed"] = printer.bed;
  if (isnan(printer.bedTarget)) doc["bedTarget"].set(nullptr); else doc["bedTarget"] = printer.bedTarget;
  doc["mic"] = micAmplitude;
  JsonArray events = doc["events"].to<JsonArray>();
  for (uint8_t i = 0; i < 12; i++) { const String &entry = eventLog[(eventHead + i) % 12]; if (entry.length()) events.add(entry); }
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSettings() {
  if (server.hasArg("wifiSsid") && server.arg("wifiSsid").length()) config.wifiSsid = server.arg("wifiSsid");
  if (server.hasArg("wifiPassword") && server.arg("wifiPassword").length()) config.wifiPassword = server.arg("wifiPassword");
  if (server.hasArg("printerSerial") && server.arg("printerSerial").length()) config.printerSerial = server.arg("printerSerial");
  if (server.hasArg("cloudUser") && server.arg("cloudUser").length()) config.cloudUser = server.arg("cloudUser");
  if (server.hasArg("cloudToken") && server.arg("cloudToken").length()) config.cloudToken = server.arg("cloudToken");
  if (server.hasArg("brightness") && server.arg("brightness").length()) config.brightness = constrain(server.arg("brightness").toInt(), 1, 255);
  if (server.hasArg("finishedSeconds") && server.arg("finishedSeconds").length()) config.finishedSeconds = constrain(server.arg("finishedSeconds").toInt(), 10, 3600);
  saveConfig();
  server.send(200, "text/html", "<p>Saved. Restarting…</p>");
  delay(500);
  ESP.restart();
}

void handlePreview() {
  String value = server.arg("mode");
  if (value == "auto") { mode = LedMode::STANDBY; updateModeFromPrinter(); }
  else if (value == "standby") { previewMode = LedMode::STANDBY; mode = LedMode::PREVIEW; initStandbyColors(true); }
  else if (value == "heating") { previewMode = LedMode::HEATING; mode = LedMode::PREVIEW; }
  else if (value == "loading") { previewMode = LedMode::FILAMENT_LOADING; mode = LedMode::PREVIEW; printer.isFilamentUnloading = false; filamentHeadPos = 0; FastLED.clear(); }
  else if (value == "unloading") { previewMode = LedMode::FILAMENT_LOADING; mode = LedMode::PREVIEW; printer.isFilamentUnloading = true; filamentHeadPos = 0; FastLED.clear(); }
  else if (value == "printing") { previewMode = LedMode::PRINTING; mode = LedMode::PREVIEW; }
  else if (value == "finished") { previewMode = LedMode::FINISHED; mode = LedMode::PREVIEW; turnOffPrinterLightBar(); }
  else { server.send(400, "text/plain", "Unknown mode"); return; }
  modeSinceMs = millis();
  server.send(204);
}

void setupWebServer() {
  server.on("/", HTTP_GET, [] { server.send(200, "text/html", cloudDashboardHtml()); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/preview", HTTP_POST, handlePreview);
  server.begin();
}
} // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("\nP1S LED controller booting");

  #if defined(ESP32)
    random16_add_entropy(esp_random());
  #else
    for (uint8_t i = 0; i < 16; i++) {
      random16_add_entropy(analogRead(A0) + micros());
      delayMicroseconds(100);
    }
  #endif

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(DEFAULT_BRIGHTNESS);
  FastLED.clear(true);
  modeSinceMs = millis();
  loadConfig();
  FastLED.setBrightness(config.brightness);
  FastLED.clear();
  FastLED.show();

  playStartupAnimation();
  mode = LedMode::STANDBY;
  modeSinceMs = millis();
  initStandbyColors();

  setupMicrophone();
  if (config.wifiSsid.length()) {
    if (!connectWifi()) {
      startSetupAp();
    }
  } else {
    startSetupAp();
  }

  mqtt.setCallback(onMqttMessage);
  setupWebServer();
  addEvent("System initialized");
}

void loop() {
  static uint32_t lastHealthLogMs = millis() - 10000;
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED && config.wifiSsid.length()) {
    static uint32_t retryWifiAt = 0;
    if (millis() - retryWifiAt > 30000) { retryWifiAt = millis(); connectWifi(); }
  }
  connectMqttIfNeeded();
  mqtt.loop();
  updateModeFromPrinter();
  renderLeds();
  if (millis() - lastHealthLogMs >= 10000) {
    lastHealthLogMs = millis();
    Serial.printf("health: wifi=%d ip=%s mqtt=%d clock=%ld mode=%s\n", WiFi.status(),
                  WiFi.localIP().toString().c_str(), mqtt.connected(),
                  static_cast<long>(time(nullptr)), modeName(mode));
  }
}