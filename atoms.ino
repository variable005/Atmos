/*
  ============================================================
   Atmos — Self-Calibrating Environmental Monitor
   (Optimized: Union Memory, NVS Flash Wear Protection, Per-Sensor Warm Boot)
  ============================================================
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>

// ---------------- Pin map & Display ----------------
#define GAS_AO_PIN   4
#define ONE_WIRE_BUS 5
#define LDR_AO_PIN   6
#define OLED_SCL_PIN 9
#define OLED_SDA_PIN 8

Adafruit_SSD1306 display(128, 64, &Wire, -1);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
Preferences nvs;

// ---------------- Timing & Thresholds ----------------
const unsigned long SAMPLE_INTERVAL_MS = 2000; 
const unsigned long CALIBRATION_DURATION_MS = 16000; 
const unsigned long NVS_SAVE_INTERVAL_MS = 3600000; 

const float Z_WARNING = 2.0;
const float Z_DANGER  = 3.5;

unsigned long lastSampleTime = 0;
unsigned long lastNVSSaveTime = 0;
unsigned long bootTime = 0;
bool isCalibrating = true;

// ---------------- Baseline structure ----------------
struct Baseline {
  float mean;
  union {
    float M2;       // Memory state 1: Used ONLY during calibration
    float variance; // Memory state 2: Used ONLY during live monitoring
  };
  int sampleCount;
  float alpha;
  float minStdDev;
  bool needsCalib;  // Tracks if this specific sensor needs a cold boot
};

// INITIALIZATION: {mean, union(M2/var), sampleCount, alpha, minStdDev, needsCalib}
Baseline gasBaseline   = {0.0, {0.0}, 0, 0.010, 5.0, true};
Baseline tempBaseline  = {0.0, {0.0}, 0, 0.005, 0.5, true};
Baseline lightBaseline = {0.0, {0.0}, 0, 0.001, 10.0, true};

enum AlertLevel { SAFE, CAUTION, DANGER };
AlertLevel currentAlert = SAFE;

// ============================================================
// NVS Storage Struct 
// ============================================================
struct NVSState {
  float mean;
  float variance;
  bool isValid;
};

// ============================================================
// Statistics (Welford's & EMA)
// ============================================================
void calibAccumulate(Baseline &b, float value) {
  b.sampleCount++;
  float delta = value - b.mean;
  b.mean += delta / b.sampleCount;
  b.M2 += delta * (value - b.mean);
}

void calibFinalize(Baseline &b) {
  float finalVar = (b.sampleCount < 2) ? (b.minStdDev * b.minStdDev) : (b.M2 / (b.sampleCount - 1));
  float minVar = b.minStdDev * b.minStdDev;
  b.variance = (finalVar < minVar) ? minVar : finalVar; 
  b.needsCalib = false; // Calibration complete for this sensor
}

void calibRollingUpdate(Baseline &b, float value) {
  float delta = value - b.mean;
  b.mean += b.alpha * delta;
  b.variance = (1.0 - b.alpha) * (b.variance + b.alpha * delta * delta);
  
  float minVar = b.minStdDev * b.minStdDev;
  if (b.variance < minVar) b.variance = minVar;
}

float zScore(Baseline &b, float value) {
  return (value - b.mean) / sqrt(b.variance);
}

// ============================================================
// NVS Management 
// ============================================================
bool loadBaselineFromNVS(const char* key, Baseline &b, float currentVal) {
  NVSState savedState;
  size_t bytesRead = nvs.getBytes(key, &savedState, sizeof(NVSState));
  
  if (bytesRead == sizeof(NVSState) && savedState.isValid) {
    // FIX: Enforce floor immediately to prevent div-by-zero during sanity check AND live monitoring
    float minVar = b.minStdDev * b.minStdDev;
    float safeVariance = (savedState.variance < minVar) ? minVar : savedState.variance;
    float expectedStdDev = sqrt(safeVariance);

    if (abs(currentVal - savedState.mean) / expectedStdDev > 5.0) {
      Serial.printf("[%s] NVS data stale. Forcing recalibration.\n", key);
      return false; 
    }
    
    b.mean = savedState.mean;
    b.variance = safeVariance;
    Serial.printf("[%s] NVS loaded. Mean: %.1f\n", key, b.mean);
    return true;
  }
  return false;
}

void saveBaselinesToNVS() {
  NVSState gState = {gasBaseline.mean, gasBaseline.variance, true};
  NVSState tState = {tempBaseline.mean, tempBaseline.variance, true};
  NVSState lState = {lightBaseline.mean, lightBaseline.variance, true};
  
  nvs.putBytes("gas", &gState, sizeof(NVSState));
  nvs.putBytes("temp", &tState, sizeof(NVSState));
  nvs.putBytes("light", &lState, sizeof(NVSState));
  Serial.println("[NVS] Baselines committed to flash.");
}

// ============================================================
// Sensor reading
// ============================================================
float readGas() { return analogRead(GAS_AO_PIN); }
float readLight() { return analogRead(LDR_AO_PIN); }

bool readTemperature(float &temperature) {
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);
  if (tempC == DEVICE_DISCONNECTED_C || isnan(tempC)) return false;
  temperature = tempC;
  return true;
}

// ============================================================
// Forward Declarations
// ============================================================
const char* alertLevelToString(AlertLevel level);
void updateDisplayCalibrating();
void updateDisplayLive(float gasValue, float gasZ, float lightValue, float lightZ, float tempValue, float tempZ, bool tempOk);

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(GAS_AO_PIN, INPUT);
  pinMode(LDR_AO_PIN, INPUT);
  pinMode(ONE_WIRE_BUS, INPUT_PULLUP); 

  sensors.begin();
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED not detected.");
  } else {
    display.clearDisplay(); display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE); display.setCursor(0,0);
    display.println("Atmos Initializing..."); display.display();
  }

  nvs.begin("atmos", false); 

  float initGas = analogRead(GAS_AO_PIN);
  float initLight = analogRead(LDR_AO_PIN);
  sensors.requestTemperatures();
  float initTemp = sensors.getTempCByIndex(0);

  // FIX: Per-sensor warm boot evaluation
  gasBaseline.needsCalib = !loadBaselineFromNVS("gas", gasBaseline, initGas);
  lightBaseline.needsCalib = !loadBaselineFromNVS("light", lightBaseline, initLight);
  tempBaseline.needsCalib = !loadBaselineFromNVS("temp", tempBaseline, initTemp);

  isCalibrating = gasBaseline.needsCalib || lightBaseline.needsCalib || tempBaseline.needsCalib;

  if (!isCalibrating) {
    Serial.println("Warm boot successful for all sensors.");
  } else {
    Serial.println("Partial or full cold boot. Calibrating missing sensors...");
    bootTime = millis();
  }
  lastSampleTime = millis();
}

// ============================================================
// Loop 
// ============================================================
void loop() {
  unsigned long now = millis();

  if (!isCalibrating && (now - lastNVSSaveTime >= NVS_SAVE_INTERVAL_MS) && currentAlert == SAFE) {
    saveBaselinesToNVS();
    lastNVSSaveTime = now;
  }

  if (now - lastSampleTime < SAMPLE_INTERVAL_MS) return;
  lastSampleTime = now;

  float gasValue = readGas();
  float lightValue = readLight();
  float tempValue = 0.0;
  bool tempOk = readTemperature(tempValue);

  if (isCalibrating) {
    // Only accumulate for sensors that actually need it
    if (gasBaseline.needsCalib) calibAccumulate(gasBaseline, gasValue);
    if (lightBaseline.needsCalib) calibAccumulate(lightBaseline, lightValue);
    if (tempOk && tempBaseline.needsCalib) calibAccumulate(tempBaseline, tempValue);

    if (now - bootTime >= CALIBRATION_DURATION_MS) {
      if (gasBaseline.needsCalib) calibFinalize(gasBaseline);
      if (lightBaseline.needsCalib) calibFinalize(lightBaseline);
      if (tempBaseline.needsCalib) calibFinalize(tempBaseline);
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
  display.println("Atmos");
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
  display.println("Atmos Monitor");

  display.setCursor(0,14);
  display.print("Gas: "); display.print(gasValue,0); display.print(" z="); display.println(gasZ,1);

  display.setCursor(0,24);
  display.print("Lgt: "); display.print(lightValue,0); display.print(" z="); display.println(lightZ,1);

  display.setCursor(0,34);
  display.print("Tmp: ");
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
