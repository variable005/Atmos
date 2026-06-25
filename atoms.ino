/*
  ============================================================
   AirGuard Cluster — Self-Calibrating Environmental Monitor
  ============================================================
  Board:    ESP32-S3 DevKitC-1
  Sensors:  MQ-2 (gas/smoke), DS18B20 (temp), LDR (light)
  Display:  SSD1306 128x64 OLED (I2C)
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- Pin map ----------------
#define GAS_AO_PIN   4
#define ONE_WIRE_BUS 5  // Changed to Pin 5 for DS18B20
#define LDR_AO_PIN   6
#define OLED_SCL_PIN 9
#define OLED_SDA_PIN 8

// ---------------- Display setup ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------- DS18B20 Setup ----------------
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ---------------- Timing ----------------
const unsigned long SAMPLE_INTERVAL_MS = 2000; 
const unsigned long CALIBRATION_DURATION_MS = 16000; 

const float EMA_ALPHA = 0.01;
const float Z_WARNING = 2.0;
const float Z_DANGER  = 3.5;

unsigned long lastSampleTime = 0;
unsigned long bootTime = 0;
bool isCalibrating = true;

// ---------------- Baseline structure ----------------
struct Baseline {
  float mean;
  float variance;
  int sampleCount;
  float sumForCalib;
  float sumSqForCalib;
};

Baseline gasBaseline   = {0,1,0,0,0};
Baseline tempBaseline  = {0,1,0,0,0};
Baseline lightBaseline = {0,1,0,0,0};

enum AlertLevel { SAFE, CAUTION, DANGER };
AlertLevel currentAlert = SAFE;

// ============================================================
// Statistics
// ============================================================
void calibAccumulate(Baseline &b, float value) {
  b.sumForCalib += value;
  b.sumSqForCalib += value * value;
  b.sampleCount++;
}

void calibFinalize(Baseline &b) {
  if (b.sampleCount < 2) {
    b.mean = b.sumForCalib / max(1, b.sampleCount);
    b.variance = 1.0;
    return;
  }
  b.mean = b.sumForCalib / b.sampleCount;
  float meanSq = b.sumSqForCalib / b.sampleCount;
  b.variance = meanSq - (b.mean * b.mean);
  if (b.variance < 1.0) b.variance = 1.0;
}

void calibRollingUpdate(Baseline &b, float value) {
  float delta = value - b.mean;
  b.mean += EMA_ALPHA * delta;
  b.variance = (1 - EMA_ALPHA) * (b.variance + EMA_ALPHA * delta * delta);
  if (b.variance < 1.0) b.variance = 1.0;
}

float zScore(Baseline &b, float value) {
  float stddev = sqrt(b.variance);
  if (stddev < 0.01) stddev = 0.01;
  return (value - b.mean) / stddev;
}

// ============================================================
// Forward declarations
// ============================================================
const char* alertLevelToString(AlertLevel level);
void updateDisplayCalibrating();
void updateDisplayLive(float gasValue, float gasZ, float lightValue, float lightZ, float tempValue, float tempZ, bool tempOk);

// ============================================================
// Sensor reading
// ============================================================
float readGas() { return analogRead(GAS_AO_PIN); }
float readLight() { return analogRead(LDR_AO_PIN); }

bool readTemperature(float &temperature) {
  sensors.requestTemperatures(); // Tell DS18B20 to grab a reading
  float tempC = sensors.getTempCByIndex(0);
  
  // DS18B20 returns DEVICE_DISCONNECTED_C (-127.0) if it fails
  if (tempC == DEVICE_DISCONNECTED_C || isnan(tempC)) {
    return false;
  }
  
  temperature = tempC;
  return true;
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("============================================");
  Serial.println(" AirGuard Cluster booting...");
  Serial.println(" DS18B20 Temperature Upgrade Configuration");
  Serial.println("============================================");

  pinMode(GAS_AO_PIN, INPUT);
  pinMode(LDR_AO_PIN, INPUT);
  
  // Internal pull-up to keep the OneWire bus stable in the simulator
  pinMode(ONE_WIRE_BUS, INPUT_PULLUP); 

  // Initialize DS18B20 Sensor bus
  sensors.begin();

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not detected.");
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0,0);
    display.println("AirGuard Cluster");
    display.println("Calibrating...");
    display.display();
  }

  bootTime = millis();
  lastSampleTime = millis();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  unsigned long now = millis();

  if (now - lastSampleTime < SAMPLE_INTERVAL_MS)
    return;

  lastSampleTime = now;

  float gasValue = readGas();
  float lightValue = readLight();
  float tempValue = 0.0;
  bool tempOk = readTemperature(tempValue);

  if (isCalibrating) {
    calibAccumulate(gasBaseline, gasValue);
    calibAccumulate(lightBaseline, lightValue);
    if (tempOk) calibAccumulate(tempBaseline, tempValue);

    Serial.print("[CALIBRATING] Gas=");
    Serial.print(gasValue);
    Serial.print(" Light=");
    Serial.print(lightValue);
    Serial.print(" Temp=");
    if (tempOk) Serial.println(tempValue); else Serial.println("n/a");

    if (now - bootTime >= CALIBRATION_DURATION_MS) {
      calibFinalize(gasBaseline);
      calibFinalize(lightBaseline);
      calibFinalize(tempBaseline);
      isCalibrating = false;
      Serial.println("Calibration complete.");
    }

    updateDisplayCalibrating();
    return;
  }

  float gasZ = zScore(gasBaseline, gasValue);
  float lightZ = zScore(lightBaseline, lightValue);
  float tempZ = tempOk ? zScore(tempBaseline, tempValue) : 0;

  float worstZ = max(gasZ, max(lightZ, tempOk ? tempZ : 0));
  AlertLevel newAlert = SAFE;

  if (worstZ >= Z_DANGER) newAlert = DANGER;
  else if (worstZ >= Z_WARNING) newAlert = CAUTION;

  currentAlert = newAlert;

  if (currentAlert == SAFE) {
    calibRollingUpdate(gasBaseline, gasValue);
    calibRollingUpdate(lightBaseline, lightValue);
    if (tempOk) calibRollingUpdate(tempBaseline, tempValue);
  }

  Serial.print("Gas="); Serial.print(gasValue); Serial.print(" z="); Serial.print(gasZ);
  Serial.print(" Light="); Serial.print(lightValue); Serial.print(" z="); Serial.print(lightZ);
  Serial.print(" Temp=");
  if (tempOk) Serial.print(tempValue); else Serial.print("n/a");
  Serial.print(" Status="); Serial.println(alertLevelToString(currentAlert));

  updateDisplayLive(gasValue, gasZ, lightValue, lightZ, tempValue, tempZ, tempOk);
}

// ============================================================
// Display Rendering Functions
// ============================================================
const char* alertLevelToString(AlertLevel level) {
  switch(level) {
    case SAFE:    return "SAFE";
    case CAUTION: return "CAUTION";
    case DANGER:  return "DANGER";
  }
  return "UNKNOWN";
}

void updateDisplayCalibrating() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("AirGuard Cluster");
  display.println("Calibrating...");
  display.print("Time left: ");
  long remaining = (CALIBRATION_DURATION_MS - (millis() - bootTime)) / 1000;
  if (remaining < 0) remaining = 0;
  display.print(remaining); display.println("s");
  display.display();
}

void updateDisplayLive(float gasValue, float gasZ, float lightValue, float lightZ, float tempValue, float tempZ, bool tempOk) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("AirGuard Cluster");

  display.setCursor(0,14);
  display.print("Gas: "); display.print(gasValue,0); display.print(" z="); display.println(gasZ,1);

  display.setCursor(0,24);
  display.print("Light: "); display.print(lightValue,0); display.print(" z="); display.println(lightZ,1);

  display.setCursor(0,34);
  display.print("Temp: ");
  if (tempOk) {
    display.print(tempValue,1); display.print("C z="); display.println(tempZ,1);
  } else {
    display.println("n/a");
  }

  display.setCursor(0,50);
  display.setTextSize(2);
  display.print(alertLevelToString(currentAlert));
  display.display();
}
