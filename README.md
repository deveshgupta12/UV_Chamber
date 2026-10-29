# ESP32 & Raspberry Pi Zero 2 W - UV Chamber System

A combined embedded power management, battery monitoring, and camera server system designed for portable inspection and UV imaging. The Raspberry Pi broadcasts its own Wi-Fi hotspot, so a phone or laptop connects directly to the chamber without needing an existing network.

## Quick Installation on Raspberry Pi Zero 2 W

Requires **Raspberry Pi OS Bookworm or newer** (the hotspot is configured through NetworkManager / `nmcli`).

Clone the repository, run the automated installer, then reboot:

```bash
git clone https://github.com/deveshgupta12/UV_Chamber.git
cd UV_Chamber
sudo bash install.sh
sudo reboot
```

> **Note:** After the reboot, the Pi **no longer joins your existing Wi-Fi network** and runs the `uvbox` hotspot instead. If you installed over Wi-Fi SSH, reconnect by joining the `uvbox` network (see [Connecting to the Chamber](#connecting-to-the-chamber)). The hotspot is not activated during installation so your SSH session is not dropped mid-install.

### What `install.sh` Does Automatically:
1. **Installs System Dependencies:** Installs `python3-picamera2`, `python3-libcamera`, `libcamera-apps`, `python3-flask`, `python3-serial`, `python3-gpiozero`, `gunicorn`, and `dnsmasq-base` (DHCP for hotspot clients).
2. **Sets Permissions:** Adds the user to the `dialout`, `gpio`, and `video` groups.
3. **Configures Hardware UART & Camera:**
   - Adds `enable_uart=1` and `camera_auto_detect=1` to `config.txt` (uses `/boot/firmware/` on Bookworm, `/boot/` on older releases).
   - Removes serial console bindings (`cmdline.txt` and `serial-getty`) to release `/dev/serial0` (GPIO 14/15) for ESP32 communication.
4. **Grants Passwordless Poweroff:** Writes `/etc/sudoers.d/010_uv_chamber_poweroff` so the server can run `sudo poweroff` for button, web, and inactivity shutdowns.
5. **Creates Image Storage:** Creates the `img/` directory for captured photos.
6. **Configures Systemd Autostart:** Installs and enables `uv_chamber.service`, which runs `final.py` on every boot and restarts it automatically if it exits.
7. **Installs the Shutdown Handshake Hook:** Installs `/lib/systemd/system-shutdown/esp32_notify.sh`, which sends `RPI:HALTED` to the ESP32 at the very end of shutdown (after filesystems are read-only) so the ESP32 knows it is safe to cut power. See [Safe Shutdown Sequence](#safe-shutdown-sequence).
8. **Configures the Wi-Fi Hotspot:** Creates the `uvbox-hotspot` NetworkManager profile and disables autoconnect on all other saved Wi-Fi networks. Re-running the installer safely replaces the existing hotspot profile.

### Connecting to the Chamber

After reboot, join the hotspot from a phone or laptop:

| Setting | Value |
|---|---|
| Wi-Fi SSID | `uvbox` |
| Password | `uvbox@123` |
| Pi IP address | `10.42.0.1` |
| Live stream | `http://10.42.0.1:5000/video_feed` |
| Battery telemetry | `http://10.42.0.1:5000/battery` |
| SSH | `ssh <user>@10.42.0.1` |

- **Change SSID / password:** Edit `HOTSPOT_SSID` and `HOTSPOT_PASSWORD` in `install.sh` and re-run `sudo bash install.sh`.
- **Revert to normal Wi-Fi client mode:**
  ```bash
  sudo nmcli connection delete uvbox-hotspot
  sudo nmcli connection modify "<your-network>" connection.autoconnect yes
  sudo reboot
  ```

### Service Management Commands:
- **Start service:** `sudo systemctl start uv_chamber.service`
- **Stop service:** `sudo systemctl stop uv_chamber.service`
- **Restart service:** `sudo systemctl restart uv_chamber.service`
- **View live logs:** `sudo journalctl -u uv_chamber.service -f`

---

## Project Architecture

- **Auxiliary Controller (ESP32-C3 Super Mini):**
  - Switches the Raspberry Pi power rail (**GPIO 8**) through a high-side P-MOSFET (`IRF4905` driven by `PN2222A`), plus two UV LED channels: **UV LED 1 – 270 nm (GPIO 10)** and **UV LED 2 – 365 nm (GPIO 7)**.
  - Reads three debounced pushbuttons (active LOW): `SW_Led1` (GPIO 4), `SW_Led2` (GPIO 3), and `SW_RPi1` (GPIO 2).
  - Monitors the 3S LiPo pack (12.6 V full) on **GPIO 1 (ADC1_CH1)** through a **42 kΩ / 10 kΩ** resistor divider (nominal ratio 5.2). The firmware uses a measured calibration ratio instead of the nominal one: 12.29 V at the battery reads 2.18 V at the pin (≈ 5.64).
  - Applies Exponential Moving Average (EMA) filtering (α = 0.1) and computes State of Charge (SOC) using an 11-point non-linear discharge lookup table (10.50 V = 0 %, 12.60 V = 100 %).
  - Streams 1 Hz telemetry and button events to the Pi over UART1 (`GPIO 21 TX`, `GPIO 20 RX`, 9600 baud). The same commands are accepted over USB serial (115200 baud) for debugging.
  - Runs a safe-shutdown state machine that cuts Pi power only after the Pi confirms it has halted.

- **Main Controller (Raspberry Pi Zero 2 W):**
  - Runs a Flask web server on port 5000 (`final.py`), reachable over its own `uvbox` Wi-Fi hotspot.
  - Dual-stream camera pipeline using `picamera2` (rotated 90°, continuous autofocus):
    - Low-resolution MJPEG live stream (`480x854`) at `/video_feed`.
    - High-resolution capture (`720x1280`) after an autofocus cycle, saved to `img/RF_pic_<timestamp>.jpeg`.
  - Reads ESP32 telemetry over `/dev/serial0` and controls the UV LEDs by sending commands to the ESP32.
  - Power management:
    - **Idle mode:** turns off both UV LEDs after 5 minutes of inactivity.
    - **Auto-shutdown:** safely powers off after 15 minutes of inactivity when no client has called `/ping` within the last 60 seconds.
  - Local Pi buttons: **GPIO 17** (toggle UV LED 1), **GPIO 21** (toggle UV LED 2), **GPIO 4** (hold 2 s to capture). Camera activity LED on **GPIO 24**.

---

## Physical Controls (ESP32 Buttons)

| Button | Action |
|---|---|
| `SW_Led1` | Toggle UV LED 1 (270 nm) |
| `SW_Led2` | Toggle UV LED 2 (365 nm) |
| `SW_RPi1` – short press | Take an autofocus capture |
| `SW_RPi1` – hold ≥ 3 s | Safe shutdown |
| `SW_RPi1` – any press while Pi is off | Power the Pi back on |

---

## UART Protocol (ESP32 ↔ Raspberry Pi)

9600 baud, 8N1, newline-terminated ASCII.

**ESP32 → Raspberry Pi**

| Message | Meaning |
|---|---|
| `VOLT:12.29,SOC:87%,LED1:0,LED2:0,RPI:1` | Telemetry (sent every second) |
| `EVENT:CAPTURE` | `SW_RPi1` short press; the Pi takes an autofocus capture |
| `EVENT:SHUTDOWN` | Shutdown requested; the Pi runs `sudo poweroff` |

**Raspberry Pi → ESP32**

| Command | Meaning |
|---|---|
| `LED1:ON` / `LED1:OFF` / `LED1:TOGGLE` | Control UV LED 1 (`LED1:1` / `LED1:0` also accepted) |
| `LED2:ON` / `LED2:OFF` / `LED2:TOGGLE` | Control UV LED 2 (`LED2:1` / `LED2:0` also accepted) |
| `LEDS:ON` / `LEDS:OFF` | Control both UV LEDs |
| `RPI:SHUTDOWN` | Start the safe shutdown sequence |
| `RPI:HALTED` | Sent by the `esp32_notify.sh` hook once the OS has fully halted |
| `RPI:ON` | Re-enable the Pi power rail (useful from the USB serial monitor) |

---

## Safe Shutdown Sequence

The ESP32 never cuts power to the Pi until Linux has finished shutting down, protecting the SD card from corruption.

1. Shutdown is triggered by holding `SW_RPi1` for 3 s, calling `/poweroff`, or the 15-minute inactivity timer. (Pi-initiated shutdowns send `RPI:SHUTDOWN` to the ESP32 first.)
2. The ESP32 turns off both UV LEDs, sends `EVENT:SHUTDOWN` to the Pi, and waits up to **25 s** for confirmation.
3. The Pi runs `sudo poweroff`. At the final stage of shutdown, `esp32_notify.sh` sends `RPI:HALTED`.
4. The ESP32 waits a further **10 s** grace period, then cuts the Pi power rail.
5. If `RPI:HALTED` is not received within 25 s, the ESP32 **aborts** the power cut and keeps the Pi powered.

Press `SW_RPi1` to power the Pi back on.

---

## HTTP API

All endpoints are served on port 5000 (`http://10.42.0.1:5000` over the hotspot).

| Endpoint | Description |
|---|---|
| `/video_feed` | MJPEG live stream |
| `/capture` (GET/POST) | Start an autofocus capture; returns `429` if one is already in progress |
| `/list_files?page=1&per_page=10` | Paginated list of captured images, newest first |
| `/images/<filename>` | Download a captured image |
| `/battery` | Voltage, SOC, LED states, Pi power state, and ESP32 connection status |
| `/uv_status` | Both UV LED states (`UV_A` = LED 1, `UV_B` = LED 2) |
| `/led1_status`, `/led2_status` | Individual UV LED state |
| `/led1_toggle`, `/led2_toggle` | Toggle a UV LED |
| `/leds_off` | Turn off both UV LEDs |
| `/ping` | Client keep-alive; resets the inactivity timer and blinks the activity LED |
| `/device_status` | Health check |
| `/poweroff` | Start the safe shutdown sequence |

---

## Directory Structure

```
├── install.sh                     # Automated setup, autostart, shutdown hook & hotspot script for Raspberry Pi
├── final.py                       # Raspberry Pi camera & telemetry server (active, run by uv_chamber.service)
├── rpi/
│   └── app.py                     # Copy of final.py
├── esp32_battery_monitor/
│   └── esp32_battery_monitor.ino  # ESP32-C3 firmware
├── img/                           # Captured images (created by install.sh / at runtime)
└── README.md
```
