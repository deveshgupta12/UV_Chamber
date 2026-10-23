#include <HardwareSerial.h>

// ADC & Resistor Divider Configuration
const int BATTERY_PIN = 1;       // GPIO 1 (ADC1_CH1, fully supported by ESP-IDF oneshot ADC)
const float R1 = 30000.0;        // R11 = 30k
const float R2 = 10000.0;        // R12 = 10k
const float DIVIDER_RATIO = (R1 + R2) / R2; // 4.0

// Control Pins from Updated Schematic
const int PIN_RPI_PWR = 8;       // GPIO 8 -> RPi Power switch (IRF4905 via PN2222A)
const int PIN_LED_1   = 10;      // GPIO 10 -> LED 1 switch
const int PIN_LED_2   = 7;       // GPIO 7 -> LED 2 switch (switched from GPIO 9)

// Hardware UART1 to Raspberry Pi
HardwareSerial RPiSerial(1);
const int TX_PIN = 21;           // GPIO 21 -> net RPI_RX -> RPi GPIO 15 (RX)
const int RX_PIN = 20;           // GPIO 20 <- net RPI_TX <- RPi GPIO 14 (TX)

// Exponential Moving Average (EMA) filter parameters
float filteredVoltage = -1.0;
const float SMOOTHING_FACTOR = 0.1; // Filter weight (0.0 to 1.0)

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

void setup() {
  Serial.begin(115200);                                  // USB CDC Serial Monitor
  RPiSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);     // Secondary UART to Raspberry Pi
  
  pinMode(BATTERY_PIN, INPUT);
  
  // Power control pins
  pinMode(PIN_RPI_PWR, OUTPUT);
  pinMode(PIN_LED_1, OUTPUT);
  pinMode(PIN_LED_2, OUTPUT);
  
  // Default states: Power on Raspberry Pi
  digitalWrite(PIN_RPI_PWR, HIGH);
  digitalWrite(PIN_LED_1, LOW);
  digitalWrite(PIN_LED_2, LOW);

  Serial.println("ESP32 Super Mini Power & Battery Monitor Initialized.");
}

void loop() {
  // 1. Read calibrated millivolts from ESP32-C3 ADC (ADC1 GPIO 1)
  uint32_t pinMilliVolts = analogReadMilliVolts(BATTERY_PIN);
  float rawVoltage = (pinMilliVolts / 1000.0) * DIVIDER_RATIO;

  // 2. Exponential Moving Average (EMA)
  if (filteredVoltage < 0) {
    filteredVoltage = rawVoltage; // Immediate snap on first reading
  } else {
    filteredVoltage = (rawVoltage * SMOOTHING_FACTOR) + (filteredVoltage * (1.0 - SMOOTHING_FACTOR));
  }

  // 3. Compute State of Charge (SOC)
  int soc = calculateSOC(filteredVoltage);

  // 4. Construct payload for Raspberry Pi
  String payload = "VOLT:" + String(filteredVoltage, 2) + ",SOC:" + String(soc) + "%\n";
  
  // Send over hardware UART to Raspberry Pi
  RPiSerial.print(payload);
  
  // Mirror to USB CDC Serial
  Serial.print("Raw: " + String(rawVoltage, 2) + "V -> ");
  Serial.print("Telemetry: " + payload);

  delay(1000);
}
