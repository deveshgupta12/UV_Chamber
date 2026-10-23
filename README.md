# ESP32 & Raspberry Pi Zero 2 W Battery & Camera System

A combined embedded power management, battery monitoring, and camera server system designed for portable inspection and UV imaging.

## Project Architecture

- **Auxiliary Controller (ESP32 Super Mini / ESP32-C3):**
  - Manages power delivery to the Raspberry Pi Zero 2 W and UV illumination circuits via high-side P-MOSFETs (`IRF4905` driven by `PN2222A`).
  - Monitors 12.6V (3S LiPo) battery pack voltage via an analog resistor divider ($30\,\text{k}\Omega / 10\,\text{k}\Omega$) on **GPIO 1 (ADC1)**.
  - Applies Exponential Moving Average (EMA) filtering and computes State of Charge (SOC) using an 11-point non-linear discharge lookup table.
  - Streams 1 Hz telemetry strings (`VOLT:xx.xx,SOC:xx%\n`) over Hardware UART (`GPIO 21 TX`, `GPIO 20 RX`) to the Raspberry Pi.

- **Main Controller (Raspberry Pi Zero 2 W):**
  - Runs a Flask web server on port 5000 (`rpi/app.py`).
  - Dual-stream camera pipeline using `picamera2`:
    - Low-resolution MJPEG live stream (`480x854`) at `/video_feed`.
    - High-resolution autofocus capture (`720x1280`) saved to disk (`img/RF_pic_...jpeg`).
  - Reads ESP32 battery telemetry over `/dev/serial0` and exposes `/battery` API endpoint.
  - Controls UV illumination and handles auto-idle / auto-shutdown power management.

## Directory Structure

```
├── esp32_battery_monitor/
│   └── esp32_battery_monitor.ino  # ESP32-C3 firmware
├── rpi/
│   └── app.py                     # Raspberry Pi Flask server & camera pipeline
└── README.md
```
