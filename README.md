# ESP32 & Raspberry Pi Zero 2 W - UV Chamber System

A combined embedded power management, battery monitoring, and camera server system designed for portable inspection and UV imaging.

## Quick Installation on Raspberry Pi Zero 2 W

Clone the repository and run the automated installer:

```bash
git clone https://github.com/deveshgupta12/UV_Chamber.git
cd UV_Chamber
sudo bash install.sh
```

### What `install.sh` Does Automatically:
1. **Installs System Dependencies:** Installs `python3-picamera2`, `libcamera`, `flask`, `pyserial`, and `gpiozero`.
2. **Configures Hardware UART:**
   - Enables `enable_uart=1` in `/boot/firmware/config.txt`.
   - Removes serial console bindings (`cmdline.txt` and `serial-getty`) to release `/dev/serial0` (GPIO 14/15) for ESP32 telemetry.
3. **Sets Permissions:** Adds user to `dialout`, `gpio`, and `video` groups; grants passwordless `sudo poweroff` permissions for auto-shutdown.
4. **Configures Systemd Autostart:** Installs, registers, and enables `uv_chamber.service` to start on every boot.

### Service Management Commands:
- **Start service:** `sudo systemctl start uv_chamber.service`
- **Stop service:** `sudo systemctl stop uv_chamber.service`
- **Restart service:** `sudo systemctl restart uv_chamber.service`
- **View live logs:** `sudo journalctl -u uv_chamber.service -f`

---

## Project Architecture

- **Auxiliary Controller (ESP32 Super Mini / ESP32-C3):**
  - Manages power delivery to the Raspberry Pi Zero 2 W and UV illumination circuits via high-side P-MOSFETs (`IRF4905` driven by `PN2222A`).
  - Monitors 12.6V (3S LiPo) battery pack voltage via an analog resistor divider ($30\,\text{k}\Omega / 10\,\text{k}\Omega$) on **GPIO 1 (ADC1)**.
  - Applies Exponential Moving Average (EMA) filtering and computes State of Charge (SOC) using an 11-point non-linear discharge lookup table.
  - Streams 1 Hz telemetry strings (`VOLT:xx.xx,SOC:xx%\n`) over Hardware UART (`GPIO 21 TX`, `GPIO 20 RX`) to the Raspberry Pi.

- **Main Controller (Raspberry Pi Zero 2 W):**
  - Runs a Flask web server on port 5000 (`final.py` / `rpi/app.py`).
  - Dual-stream camera pipeline using `picamera2`:
    - Low-resolution MJPEG live stream (`480x854`) at `/video_feed`.
    - High-resolution autofocus capture (`720x1280`) saved to disk (`img/RF_pic_...jpeg`).
  - Reads ESP32 battery telemetry over `/dev/serial0` and exposes `/battery` API endpoint.
  - Controls UV illumination and handles auto-idle / auto-shutdown power management.

---

## Directory Structure

```
├── install.sh                     # Automated setup and autostart script for Raspberry Pi
├── final.py                       # Raspberry Pi camera & telemetry server (active)
├── rpi/
│   └── app.py                     # Modular copy of RPi server
├── esp32_battery_monitor/
│   └── esp32_battery_monitor.ino  # ESP32-C3 firmware
└── README.md
```
