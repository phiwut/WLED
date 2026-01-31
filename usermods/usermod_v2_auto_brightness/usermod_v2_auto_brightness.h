#pragma once

#include "wled.h"
#include <BH1750.h>

class AutoBrightnessUsermod : public Usermod {
  private:
    BH1750 lightMeter;
    bool autoBrightnessEnabled = false;
    int  autoBriMinPercent = 10;           // minimum brightness percentage (0-100)
    int  autoBriMaxPercent = 100;          // maximum brightness percentage (1-100)
    int  autoBriMinLux = 5;               // lux reading considered "darkest" (maps to min brightness)
    int  autoBriMaxLux = 500;             // lux reading considered "fully bright" (maps to max brightness)
    bool sensorFound = false;
    bool sensorInitDone = false;

    float lastLux = -1.0f;
    float smoothedLux = -1.0f;
    byte  autoBriTarget = 128;
    byte  autoBriCurrent = 128;
    unsigned long lastSensorRead = 0;
    unsigned long lastBriUpdate = 0;
    unsigned long lastUiSync = 0;          // last time we synced UI via stateUpdated

    static const unsigned long SENSOR_READ_INTERVAL = 500;   // read sensor every 0.5s
    static const unsigned long BRI_STEP_INTERVAL = 30;       // smooth step every 30ms
    static const unsigned long UI_SYNC_INTERVAL = 2000;      // sync UI slider every 2s
    static constexpr float EMA_ALPHA = 0.35f;                // EMA smoothing factor

    // Map lux to brightness using sqrt curve for perceptual linearity
    byte luxToBrightness(float lux) {
      byte minBri = (byte)((autoBriMinPercent * 255) / 100);
      if (minBri < 1) minBri = 1;
      int maxBri = (autoBriMaxPercent * 255) / 100;
      if (maxBri < (int)minBri) maxBri = (int)minBri;

      float fMinLux = (float)autoBriMinLux;
      float fMaxLux = (float)autoBriMaxLux;
      if (fMaxLux <= fMinLux) fMaxLux = fMinLux + 1.0f;

      // Below min lux -> min brightness
      if (lux <= fMinLux) return minBri;
      // Above max lux -> max brightness
      if (lux >= fMaxLux) return (byte)maxBri;

      // Normalize lux into [0..1] range between min and max
      float normalized = (lux - fMinLux) / (fMaxLux - fMinLux);
      // Apply sqrt curve for perceptual linearity
      normalized = sqrtf(normalized);

      return minBri + (byte)(normalized * ((float)maxBri - (float)minBri));
    }

    // Advance one step of smooth brightness interpolation
    void stepBrightness() {
      if (autoBriCurrent == autoBriTarget) return;

      int diff = (int)autoBriTarget - (int)autoBriCurrent;
      int absDiff = abs(diff);
      int step;
      if (absDiff > 50) step = 4;
      else if (absDiff > 20) step = 2;
      else step = 1;

      if (diff > 0) {
        autoBriCurrent = (byte)min((int)autoBriCurrent + step, (int)autoBriTarget);
      } else {
        autoBriCurrent = (byte)max((int)autoBriCurrent - step, (int)autoBriTarget);
      }
    }

  public:
    void setup() {
      // Initialize BH1750 if I2C pins are configured
      if (i2c_scl >= 0 && i2c_sda >= 0) {
        sensorFound = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
        DEBUG_PRINTLN(sensorFound ? F("AutoBrightness: BH1750 found") : F("AutoBrightness: BH1750 not found"));
      }
      sensorInitDone = true;
      autoBriCurrent = bri;
      autoBriTarget = bri;
    }

    void loop() {
      if (strip.isUpdating()) return;

      unsigned long now = millis();

      // --- Auto-brightness: read sensor ---
      if (autoBrightnessEnabled && sensorFound && (now - lastSensorRead >= SENSOR_READ_INTERVAL)) {
        float lux = lightMeter.readLightLevel();
        lastSensorRead = now;

        if (lux >= 0) {
          lastLux = lux;
          if (smoothedLux < 0) {
            smoothedLux = lux;       // first reading
            autoBriCurrent = bri;    // sync with current brightness
          } else {
            smoothedLux = EMA_ALPHA * lux + (1.0f - EMA_ALPHA) * smoothedLux;
          }
          byte newTarget = luxToBrightness(smoothedLux);
          if (abs((int)newTarget - (int)autoBriTarget) > 3) {
            autoBriTarget = newTarget;
          }
          static byte debugLogCounter = 0;
          if (++debugLogCounter >= 5) {
            debugLogCounter = 0;
            DEBUG_PRINTF("AutoBrightness: lux=%.1f lx smoothed=%.1f target_bri=%d current_bri=%d\n", lux, smoothedLux, (int)newTarget, (int)autoBriCurrent);
          }
        } else {
          DEBUG_PRINTLN(F("AutoBrightness: BH1750 read error (lux < 0)"));
        }
      }

      // --- Auto-brightness: step toward target ---
      if (autoBrightnessEnabled && sensorFound && !transitionActive && (now - lastBriUpdate >= BRI_STEP_INTERVAL)) {
        lastBriUpdate = now;
        byte prev = autoBriCurrent;
        stepBrightness();
        if (autoBriCurrent != prev) {
          bri = autoBriCurrent;
          briOld = bri;
          briT = bri;
          applyBri();
          if (bri > 0) briLast = bri;

          // Periodically notify UI so the brightness slider updates on the main page
          if (now - lastUiSync >= UI_SYNC_INTERVAL) {
            lastUiSync = now;
            interfaceUpdateCallMode = CALL_MODE_NO_NOTIFY;
          }
        }
      }
    }

    void addToJsonInfo(JsonObject& root) {
      if (!sensorFound) return;

      JsonObject user = root[F("u")];
      if (user.isNull()) user = root.createNestedObject(F("u"));

      JsonArray luxArr = user.createNestedArray(F("Helligkeit (Lux)"));
      if (lastLux >= 0) {
        luxArr.add(roundf(lastLux * 10.0f) / 10.0f);
        luxArr.add(F(" lx"));
      } else {
        luxArr.add(F("warte..."));
      }

      if (autoBrightnessEnabled) {
        JsonArray briArr = user.createNestedArray(F("Auto-Helligkeit"));
        briArr.add(autoBriTarget);
        int minBri = (autoBriMinPercent * 255) / 100;
        if (minBri < 1) minBri = 1;
        int maxBri = (autoBriMaxPercent * 255) / 100;
        if (maxBri < minBri) maxBri = minBri;
        briArr.add(String(F("/")) + maxBri);
      }
    }

    void addToConfig(JsonObject& root) {
      JsonObject top = root.createNestedObject(F("Auto-Helligkeit"));
      top[F("Aktiv")] = autoBrightnessEnabled;
      top[F("Min Helligkeit %")] = autoBriMinPercent;
      top[F("Max Helligkeit %")] = autoBriMaxPercent;
      top[F("Min Lux")] = autoBriMinLux;
      top[F("Max Lux")] = autoBriMaxLux;
    }

    void appendConfigData() {
      oappend(SET_F("addInfo('Auto-Helligkeit:Aktiv', 1, 'BH1750 Sensor');"));
      oappend(SET_F("addInfo('Auto-Helligkeit:Min Helligkeit %', 1, '0-100, Standard: 10');"));
      oappend(SET_F("addInfo('Auto-Helligkeit:Max Helligkeit %', 1, '1-100, Standard: 100');"));
      oappend(SET_F("addInfo('Auto-Helligkeit:Min Lux', 1, 'Lux fuer min. Helligkeit (dunkelster Raum)');"));
      oappend(SET_F("addInfo('Auto-Helligkeit:Max Lux', 1, 'Lux fuer max. Helligkeit (hellster Raum)');"));
    }

    bool readFromConfig(JsonObject& root) {
      JsonObject top = root[F("Auto-Helligkeit")];

      bool configComplete = !top.isNull();

      configComplete &= getJsonValue(top[F("Aktiv")], autoBrightnessEnabled);
      configComplete &= getJsonValue(top[F("Min Helligkeit %")], autoBriMinPercent);
      configComplete &= getJsonValue(top[F("Max Helligkeit %")], autoBriMaxPercent);
      configComplete &= getJsonValue(top[F("Min Lux")], autoBriMinLux);
      configComplete &= getJsonValue(top[F("Max Lux")], autoBriMaxLux);

      if (autoBriMinPercent < 0) autoBriMinPercent = 0;
      if (autoBriMinPercent > 100) autoBriMinPercent = 100;
      if (autoBriMaxPercent < 1) autoBriMaxPercent = 1;
      if (autoBriMaxPercent > 100) autoBriMaxPercent = 100;
      if (autoBriMaxPercent < autoBriMinPercent) autoBriMaxPercent = autoBriMinPercent;
      if (autoBriMinLux < 0) autoBriMinLux = 0;
      if (autoBriMinLux > 65535) autoBriMinLux = 65535;
      if (autoBriMaxLux < 1) autoBriMaxLux = 1;
      if (autoBriMaxLux > 65535) autoBriMaxLux = 65535;

      // Reset smoothing state when auto-brightness is toggled off
      if (!autoBrightnessEnabled) {
        smoothedLux = -1.0f;
      }

      return configComplete;
    }

    uint16_t getId() {
      return USERMOD_ID_AUTO_BRIGHTNESS;
    }
};
