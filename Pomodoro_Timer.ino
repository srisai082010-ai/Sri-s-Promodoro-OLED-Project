/*
  Pomodoro OLED Timer — XIAO ESP32-S3
  Hardware: SSD1306 128x64 I2C OLED (0x3C), EC11 encoder + switch,
            transistor-driven 3.3V ACTIVE buzzer.

  Pins (ESP32 GPIO numbers, matching the KiCad schematic):
    ENC_A = GPIO1  (XIAO D0)
    ENC_B = GPIO2  (XIAO D1)
    ENC_SW = GPIO4 (XIAO D3)
    SDA = GPIO5    (XIAO D4)
    SCL = GPIO6    (XIAO D5)
    BUZZER = GPIO7 (XIAO D8)

  Controls:
    Rotate: select work/break minutes in setup.
    Click: confirm work -> confirm break -> start; pause/resume while running.
    Hold button ~1.5 seconds: reset to setup without clearing session count.

  Requires Arduino libraries: Adafruit SSD1306, Adafruit GFX Library.
  ESP32 board package provides Preferences.h.
  This code is a design prototype; test with physical hardware before claiming it works.
*/
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

constexpr uint8_t PIN_ENC_A = 1;
constexpr uint8_t PIN_ENC_B = 2;
constexpr uint8_t PIN_BUTTON = 4;
constexpr uint8_t PIN_SDA = 5;
constexpr uint8_t PIN_SCL = 6;
constexpr uint8_t PIN_BUZZER = 7;

constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 64;
constexpr uint8_t OLED_ADDR = 0x3C; // Some modules use 0x3D.
constexpr uint32_t LONG_PRESS_MS = 1500;
constexpr uint32_t DEBOUNCE_MS = 35;
constexpr uint32_t DRAW_PERIOD_MS = 100;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Preferences preferences;

enum class Mode : uint8_t { SET_WORK, SET_BREAK, WORK, BREAK };
Mode mode = Mode::SET_WORK;
uint8_t workMinutes = 25;
uint8_t breakMinutes = 5;
uint32_t sessions = 0;
uint32_t remainingSeconds = 25 * 60;
uint32_t lastTimerMs = 0;
uint32_t lastDrawMs = 0;
uint32_t lastMovementMs = 0;
uint32_t lastBeepMs = 0;
bool paused = false;
bool buzzerOn = false;
bool displayReady = false;
uint8_t movementIndex = 0;

// Rotary decoder: each valid quadrature transition adds +1 or -1;
// most encoders take four steps per physical click.
uint8_t previousAB = 0;
int8_t encoderSum = 0;

bool rawButton = HIGH;
bool stableButton = HIGH;
uint32_t lastButtonChange = 0;
uint32_t buttonPressedAt = 0;
bool longPressHandled = false;

void beep(uint32_t now) {
  digitalWrite(PIN_BUZZER, HIGH); // Transistor switches active buzzer on.
  buzzerOn = true;
  lastBeepMs = now;
}

void goToSetup() {
  mode = Mode::SET_WORK;
  paused = false;
  remainingSeconds = uint32_t(workMinutes) * 60;
  digitalWrite(PIN_BUZZER, LOW);
  buzzerOn = false;
  encoderSum = 0;
}

void advanceMode(uint32_t now) {
  switch (mode) {
    case Mode::SET_WORK:
      mode = Mode::SET_BREAK;
      remainingSeconds = uint32_t(breakMinutes) * 60;
      break;
    case Mode::SET_BREAK:
      mode = Mode::WORK;
      remainingSeconds = uint32_t(workMinutes) * 60;
      paused = false;
      lastTimerMs = now;
      break;
    case Mode::WORK:
    case Mode::BREAK:
      paused = !paused;
      lastTimerMs = now;
      break;
  }
}

void pollButton(uint32_t now) {
  const bool reading = digitalRead(PIN_BUTTON);
  if (reading != rawButton) {
    rawButton = reading;
    lastButtonChange = now;
  }
  if (now - lastButtonChange >= DEBOUNCE_MS && reading != stableButton) {
    stableButton = reading;
    if (stableButton == LOW) {
      buttonPressedAt = now;
      longPressHandled = false;
    } else if (!longPressHandled) {
      advanceMode(now);
    }
  }
  if (stableButton == LOW && !longPressHandled &&
      now - buttonPressedAt >= LONG_PRESS_MS) {
    longPressHandled = true;
    goToSetup();
  }
}

void pollEncoder() {
  const uint8_t currentAB = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  if (currentAB == previousAB) return;
  // Index = previous state * 4 + current state, values for quadrature sequence.
  static const int8_t delta[16] = {
     0, -1, +1,  0,
    +1,  0,  0, -1,
    -1,  0,  0, +1,
     0, +1, -1,  0
  };
  encoderSum += delta[(previousAB << 2) | currentAB];
  previousAB = currentAB;

  if (mode != Mode::SET_WORK && mode != Mode::SET_BREAK) {
    encoderSum = 0;
    return;
  }
  if (encoderSum >= 4 || encoderSum <= -4) {
    const int8_t step = encoderSum > 0 ? 1 : -1;
    encoderSum = 0;
    if (mode == Mode::SET_WORK) {
      workMinutes = constrain(int(workMinutes) + step, 1, 120);
      remainingSeconds = uint32_t(workMinutes) * 60;
    } else {
      breakMinutes = constrain(int(breakMinutes) + step, 1, 60);
      remainingSeconds = uint32_t(breakMinutes) * 60;
    }
  }
}

void finishSession(uint32_t now) {
  beep(now);
  if (mode == Mode::WORK) {
    ++sessions;
    preferences.putUInt("sessions", sessions); // Only write when a session finishes.
    mode = Mode::BREAK;
    remainingSeconds = uint32_t(breakMinutes) * 60;
  } else {
    mode = Mode::WORK;
    remainingSeconds = uint32_t(workMinutes) * 60;
  }
  lastTimerMs = now;
}

void updateTimer(uint32_t now) {
  if (mode != Mode::WORK && mode != Mode::BREAK) return;
  if (paused) {
    lastTimerMs = now;
    return;
  }
  const uint32_t elapsedSeconds = (now - lastTimerMs) / 1000;
  if (elapsedSeconds == 0) return;
  lastTimerMs += elapsedSeconds * 1000;
  if (elapsedSeconds >= remainingSeconds) {
    finishSession(now);
  } else {
    remainingSeconds -= elapsedSeconds;
  }
}

void drawScreen(uint32_t now) {
  if (!displayReady || now - lastDrawMs < DRAW_PERIOD_MS) return;
  lastDrawMs = now;
  if (now - lastMovementMs >= 20000) {
    lastMovementMs = now;
    movementIndex = (movementIndex + 1) % 4;
  }
  static const int8_t offsetX[4] = {0, 3, 0, -3};
  static const int8_t offsetY[4] = {0, 0, 2, 0};

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (mode == Mode::SET_WORK) display.print("SET WORK MINUTES");
  else if (mode == Mode::SET_BREAK) display.print("SET BREAK MINUTES");
  else if (mode == Mode::WORK) display.print(paused ? "WORK - PAUSED" : "WORK SESSION");
  else display.print(paused ? "BREAK - PAUSED" : "BREAK TIME");

  const uint32_t minutes = remainingSeconds / 60;
  const uint32_t seconds = remainingSeconds % 60;
  char timeText[8];
  snprintf(timeText, sizeof(timeText), "%02lu:%02lu",
           static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
  display.setTextSize(3);
  display.setCursor(17 + offsetX[movementIndex], 17 + offsetY[movementIndex]);
  display.print(timeText);

  display.setTextSize(1);
  display.setCursor(0, 50);
  display.print("Done: ");
  display.print(sessions);
  display.setCursor(0, 58);
  if (mode == Mode::SET_WORK || mode == Mode::SET_BREAK)
    display.print("Turn=change  Press=next");
  else
    display.print("Press=pause Hold=reset");
  display.display();
}

void setup() {
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  previousAB = (digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B);
  rawButton = stableButton = digitalRead(PIN_BUTTON);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  preferences.begin("pomodoro", false);
  sessions = preferences.getUInt("sessions", 0);
  lastTimerMs = lastDrawMs = lastMovementMs = millis();
}

void loop() {
  const uint32_t now = millis();
  pollEncoder();
  pollButton(now);
  updateTimer(now);
  if (buzzerOn && now - lastBeepMs >= 250) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerOn = false;
  }
  drawScreen(now);
  delay(1);
}
