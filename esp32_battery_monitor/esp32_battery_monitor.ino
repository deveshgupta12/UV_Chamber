#include <HardwareSerial.h>

// ==============================================================================
// PIN DEFINITIONS (Schematic Mapped)
// ==============================================================================

// Battery Voltage Sensing
const int BATTERY_PIN = 1;       // GPIO 1 (ADC1_CH1)
// Calibrated ratio: 12.29V input produces 2.18V at GPIO 1
const float DIVIDER_RATIO = 12.29 / 2.18; 

// Power & LED Outputs
const int PIN_RPI_PWR = 8;       // GPIO 8 -> RPi Power switch (IRF4905 via PN2222A)
const int PIN_LED_1   = 10;      // GPIO 10 -> UV LED 1 (270nm) switch
const int PIN_LED_2   = 7;       // GPIO 7 -> UV LED 2 (365nm) switch

// Pushbutton Inputs (Active LOW with external 10k pull-ups R8, R9, R10)
const int BTN_LED1    = 4;       // GPIO 4 -> SW_Led1 (UV LED 1 toggle)
const int BTN_LED2    = 3;       // GPIO 3 -> SW_Led2 (UV LED 2 toggle)
const int BTN_RPI     = 2;       // GPIO 2 -> SW_RPi1 (Power On / Capture / Shutdown)

// Hardware UART1 to Raspberry Pi (GPIO 14 TX, GPIO 15 RX)
HardwareSerial RPiSerial(1);
const int TX_PIN = 21;           // GPIO 21 -> net RPI_RX -> RPi GPIO 15 (RX)
const int RX_PIN = 20;           // GPIO 20 <- net RPI_TX <- RPi GPIO 14 (TX)

// ==============================================================================
// STATE DEFINITIONS & TIMERS
// ==============================================================================

bool led1State = false;
bool led2State = false;
bool rpiPowerState = true;

// Shutdown State Machine
enum ShutdownState {
  STATE_IDLE,                 // Normal operation
  STATE_WAITING_FOR_HALT,     // Shutdown command sent; waiting for RPi to signal 'RPI:HALTED'
  STATE_POST_HALT_DELAY       // 'RPI:HALTED' confirmed; waiting 10s grace period before cutting power
};

ShutdownState shutdownState = STATE_IDLE;

// Timing Constants
const unsigned long HALT_TIMEOUT_MS    = 25000; // 25s timeout to receive 'RPI:HALTED'
const unsigned long POST_HALT_DELAY_MS = 10000; // 10s wait after halt signal before cutting power
unsigned long shutdownTimer = 0;

// Debounce timing
const unsigned long DEBOUNCE_DELAY_MS = 50;
const unsigned long LONG_PRESS_DELAY_MS = 3000; // 3 sec hold on SW_RPi1 triggers shutdown

// Button 1 (LED 1)
int lastBtn1Reading = HIGH;
int btn1State = HIGH;
unsigned long lastBtn1DebounceTime = 0;

// Button 2 (LED 2)
int lastBtn2Reading = HIGH;
int btn2State = HIGH;
unsigned long lastBtn2DebounceTime = 0;

// Button 3 (RPi / Capture)
int lastBtnRpiReading = HIGH;
int btnRpiState = HIGH;
unsigned long lastBtnRpiDebounceTime = 0;
unsigned long btnRpiPressStartTime = 0;
bool btnRpiLongPressHandled = false;

// Battery filter & telemetry
float filteredVoltage = -1.0;
const float SMOOTHING_FACTOR = 0.1;
unsigned long lastTelemetryTime = 0;

// ==============================================================================
// HELPER FUNCTIONS
// ==============================================================================

// 11-point SOC lookup for 3S LiPo battery
int calculateSOC(float voltage) {
  const float volts[] = {10.50, 11.10, 11.31, 11.43, 11.55, 11.67, 11.82, 11.97, 12.15, 12.36, 12.60};
  const int socs[]    = {   0,    10,    20,    30,    40,    50,    60,    70,    80,    90,   100};

  if (voltage >= volts[10]) return 100;
  if (voltage <= volts[0])  return 0;

  for (int i = 0; i < 10; i++) {
    if (voltage >= volts[i] && voltage <= volts[i+1]) {
      float frac = (voltage - volts[i]) / (volts[i+1] - volts[i]);
      return socs[i] + (int)(frac * (socs[i+1] - socs[i]));
    }
  }
  return 0;
}

void applyLEDStates() {
  digitalWrite(PIN_LED_1, led1State ? HIGH : LOW);
  digitalWrite(PIN_LED_2, led2State ? HIGH : LOW);
}

void powerOnRPi() {
  shutdownState = STATE_IDLE;
  rpiPowerState = true;
  digitalWrite(PIN_RPI_PWR, HIGH);
  Serial.println("[POWER] Raspberry Pi Power Rail turned ON (Q6 MOSFET Active).");
}

void sendEventToAll(const String& event) {
  RPiSerial.print(event);
  Serial.print(event); // Mirrors raw event to USB Serial as well
}

void triggerShutdown() {
  if (!rpiPowerState || shutdownState != STATE_IDLE) return;
  
  // Enter waiting state
  shutdownState = STATE_WAITING_FOR_HALT;
  shutdownTimer = millis();

  // Turn off UV LEDs to reduce load during shutdown
  led1State = false;
  led2State = false;
  applyLEDStates();

  // Send shutdown request to both Raspberry Pi UART and USB
  sendEventToAll("EVENT:SHUTDOWN\n");
  Serial.println(String("[SHUTDOWN @") + millis() + "ms] Shutdown initiated. Waiting for 'RPI:HALTED' (Timeout: 25s)...");
}

void processIncomingCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  // During post-halt delay, ignore ALL commands — the Pi is halting and the UART
  // line may produce noise/garbage that we must discard.
  if (shutdownState == STATE_POST_HALT_DELAY) {
    Serial.println(String("[SHUTDOWN @") + millis() + "ms] Ignoring command during post-halt grace period: " + cmd);
    return;
  }

  if (cmd == "LED1:ON" || cmd == "LED1:1") {
    led1State = true;
    applyLEDStates();
    Serial.println("[CMD] UV LED 1 -> ON");
  } else if (cmd == "LED1:OFF" || cmd == "LED1:0") {
    led1State = false;
    applyLEDStates();
    Serial.println("[CMD] UV LED 1 -> OFF");
  } else if (cmd == "LED1:TOGGLE") {
    led1State = !led1State;
    applyLEDStates();
    Serial.println(String("[CMD] UV LED 1 -> TOGGLE (now ") + (led1State ? "ON)" : "OFF)"));
  } else if (cmd == "LED2:ON" || cmd == "LED2:1") {
    led2State = true;
    applyLEDStates();
    Serial.println("[CMD] UV LED 2 -> ON");
  } else if (cmd == "LED2:OFF" || cmd == "LED2:0") {
    led2State = false;
    applyLEDStates();
    Serial.println("[CMD] UV LED 2 -> OFF");
  } else if (cmd == "LED2:TOGGLE") {
    led2State = !led2State;
    applyLEDStates();
    Serial.println(String("[CMD] UV LED 2 -> TOGGLE (now ") + (led2State ? "ON)" : "OFF)"));
  } else if (cmd == "LEDS:OFF" || cmd == "LEDS:0") {
    led1State = false;
    led2State = false;
    applyLEDStates();
    Serial.println("[CMD] All UV LEDs -> OFF");
  } else if (cmd == "LEDS:ON" || cmd == "LEDS:1") {
    led1State = true;
    led2State = true;
    applyLEDStates();
    Serial.println("[CMD] All UV LEDs -> ON");
  } else if (cmd == "RPI:SHUTDOWN") {
    triggerShutdown();
  } else if (cmd == "RPI:HALTED") {
    // The Raspberry Pi OS has finished unmounting and reached the final system-shutdown hook
    if (shutdownState == STATE_WAITING_FOR_HALT) {
      shutdownState = STATE_POST_HALT_DELAY;
      shutdownTimer = millis();
      Serial.println(String("[SHUTDOWN @") + shutdownTimer + "ms] 'RPI:HALTED' confirmed! Starting 10s grace period...");
      // Drain any remaining bytes in the UART buffer — during halt the line may produce garbage
      while (RPiSerial.available()) RPiSerial.read();
    } else {
      Serial.println(String("[SHUTDOWN @") + millis() + "ms] Received 'RPI:HALTED' but not in WAITING_FOR_HALT state (state=" + shutdownState + "). Ignored.");
    }
  } else if (cmd == "RPI:ON") {
    powerOnRPi();
  }
}

// ==============================================================================
// BUTTON HANDLERS
// ==============================================================================

void handleButtons() {
  unsigned long now = millis();

  // --- Button 1 (SW_Led1 / GPIO 4) ---
  int read1 = digitalRead(BTN_LED1);
  if (read1 != lastBtn1Reading) {
    lastBtn1DebounceTime = now;
  }
  if ((now - lastBtn1DebounceTime) > DEBOUNCE_DELAY_MS) {
    if (read1 != btn1State) {
      btn1State = read1;
      if (btn1State == LOW) { // Pressed
        led1State = !led1State;
        applyLEDStates();
        Serial.println(String("[BUTTON] SW_Led1 pressed -> UV LED 1 toggled to ") + (led1State ? "ON" : "OFF"));
      }
    }
  }
  lastBtn1Reading = read1;

  // --- Button 2 (SW_Led2 / GPIO 3) ---
  int read2 = digitalRead(BTN_LED2);
  if (read2 != lastBtn2Reading) {
    lastBtn2DebounceTime = now;
  }
  if ((now - lastBtn2DebounceTime) > DEBOUNCE_DELAY_MS) {
    if (read2 != btn2State) {
      btn2State = read2;
      if (btn2State == LOW) { // Pressed
        led2State = !led2State;
        applyLEDStates();
        Serial.println(String("[BUTTON] SW_Led2 pressed -> UV LED 2 toggled to ") + (led2State ? "ON" : "OFF"));
      }
    }
  }
  lastBtn2Reading = read2;

  // --- Button 3 (SW_RPi1 / GPIO 2) ---
  int readRpi = digitalRead(BTN_RPI);
  if (readRpi != lastBtnRpiReading) {
    lastBtnRpiDebounceTime = now;
  }
  if ((now - lastBtnRpiDebounceTime) > DEBOUNCE_DELAY_MS) {
    if (readRpi != btnRpiState) {
      btnRpiState = readRpi;
      if (btnRpiState == LOW) { // Pressed down
        btnRpiPressStartTime = now;
        btnRpiLongPressHandled = false;

        // If RPi is currently powered OFF, any press powers it back ON!
        if (!rpiPowerState) {
          powerOnRPi();
          btnRpiLongPressHandled = true;
        }
      } else { // Released
        if (rpiPowerState && !btnRpiLongPressHandled) {
          // Short press while RPi is ON: trigger autofocus capture on Raspberry Pi
          sendEventToAll("EVENT:CAPTURE\n");
          Serial.println("[BUTTON] SW_RPi1 short press -> Sent EVENT:CAPTURE to Raspberry Pi");
        }
      }
    }
    // Check for long press while RPi is ON (hold for >3 seconds to trigger shutdown)
    if (rpiPowerState && btnRpiState == LOW && !btnRpiLongPressHandled) {
      if (now - btnRpiPressStartTime >= LONG_PRESS_DELAY_MS) {
        btnRpiLongPressHandled = true;
        Serial.println("[BUTTON] SW_RPi1 long press (>3s) -> Triggering System Shutdown");
        triggerShutdown();
      }
    }
  }
  lastBtnRpiReading = readRpi;
}

// ==============================================================================
// SETUP & LOOP
// ==============================================================================

void setup() {
  Serial.begin(115200);                                  // USB CDC Serial Monitor
  RPiSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);     // Secondary UART to Raspberry Pi
  
  pinMode(BATTERY_PIN, INPUT);
  
  // Power & LED Outputs
  pinMode(PIN_RPI_PWR, OUTPUT);
  pinMode(PIN_LED_1, OUTPUT);
  pinMode(PIN_LED_2, OUTPUT);
  
  // Pushbutton Inputs (with internal pullups enabled as extra safety)
  pinMode(BTN_LED1, INPUT_PULLUP);
  pinMode(BTN_LED2, INPUT_PULLUP);
  pinMode(BTN_RPI, INPUT_PULLUP);

  // Power on Raspberry Pi on boot
  powerOnRPi();
  applyLEDStates();

  Serial.println("ESP32 Super Mini Power, Battery, LEDs & Handshake Controller Initialized.");
}

void loop() {
  unsigned long now = millis();

  // 1. Service Physical Pushbuttons
  handleButtons();

  // 2. Process incoming commands from Raspberry Pi UART (Hardware UART1)
  while (RPiSerial.available()) {
    String cmd = RPiSerial.readStringUntil('\n');
    if (cmd.length() > 0) {
      Serial.print("[RPi->ESP32 UART]: ");
      Serial.println(cmd);
      processIncomingCommand(cmd);
    }
  }

  // 3. Process commands from USB CDC Serial Monitor for manual debugging / direct control
  while (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    if (cmd.length() > 0) {
      Serial.print("[USB->ESP32]: ");
      Serial.println(cmd);
      processIncomingCommand(cmd);
    }
  }

  // 4. Safe Shutdown State Machine
  now = millis(); // Refresh timestamp so it is always >= timestamps set in step 2 or 3
  if (shutdownState == STATE_WAITING_FOR_HALT) {
    // Check if timeout elapsed without receiving 'RPI:HALTED'
    if (now >= shutdownTimer && (now - shutdownTimer >= HALT_TIMEOUT_MS)) {
      shutdownState = STATE_IDLE; // Cancel shutdown
      // Keep PIN_RPI_PWR HIGH!
      Serial.println("=================================================================");
      Serial.println(String("[ERROR / SAFETY @") + now + "ms] Shutdown TIMEOUT! 'RPI:HALTED' not received.");
      Serial.println("[SAFETY ALERT] Power cut ABORTED to protect Raspberry Pi filesystem!");
      Serial.println("=================================================================");
    }
  } else if (shutdownState == STATE_POST_HALT_DELAY) {
    // 7.5 seconds grace period after RPi reported HALTED
    if (now >= shutdownTimer) {
      unsigned long elapsed = now - shutdownTimer;
      if (elapsed >= POST_HALT_DELAY_MS) {
        Serial.println(String("[POWER @") + now + "ms] 10s post-halt elapsed (actual: " + elapsed + "ms). Cutting RPi power (Q6 MOSFET OFF).");
        digitalWrite(PIN_RPI_PWR, LOW);
        rpiPowerState = false;
        shutdownState = STATE_IDLE;
      }
    }
  }

  // 5. Periodic telemetry transmission every 1000ms
  if (now - lastTelemetryTime >= 1000) {
    lastTelemetryTime = now;

    // Read ADC millivolts
    uint32_t pinMilliVolts = analogReadMilliVolts(BATTERY_PIN);
    float rawVoltage = (pinMilliVolts / 1000.0) * DIVIDER_RATIO;

    // EMA smoothing
    if (filteredVoltage < 0) {
      filteredVoltage = rawVoltage;
    } else {
      filteredVoltage = (rawVoltage * SMOOTHING_FACTOR) + (filteredVoltage * (1.0 - SMOOTHING_FACTOR));
    }

    int soc = calculateSOC(filteredVoltage);

    // Standard telemetry packet: "VOLT:12.29,SOC:87%,LED1:0,LED2:0,RPI:1\n"
    String payload = "VOLT:" + String(filteredVoltage, 2) + 
                     ",SOC:" + String(soc) + 
                     "%,LED1:" + String(led1State ? 1 : 0) + 
                     ",LED2:" + String(led2State ? 1 : 0) + 
                     ",RPI:" + String(rpiPowerState ? 1 : 0) + "\n";
    
    // Transmit exact same packet on both Raspberry Pi UART and USB CDC Serial
    RPiSerial.print(payload);
    Serial.print(payload);
  }
}
