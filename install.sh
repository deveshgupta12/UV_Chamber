#!/usr/bin/env bash
# ==============================================================================
# UV Chamber - Raspberry Pi Zero 2 W Automated Setup & Auto-Start Script
# ==============================================================================

set -e

# Ensure script is run with root privileges
if [ "$EUID" -ne 0 ]; then
  echo "[-] Please run this installer with sudo: sudo bash install.sh"
  exit 1
fi

# Determine the actual non-root username and project path
TARGET_USER="${SUDO_USER:-$USER}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=========================================================="
echo " Starting Installation for UV Chamber on Raspberry Pi"
echo " User: $TARGET_USER"
echo " Project Directory: $PROJECT_DIR"
echo "=========================================================="

# 1. Update Package Lists
echo "[+] Updating apt repositories..."
apt-get update -y

# 2. Install Required Dependencies (System + Python)
echo "[+] Installing camera, serial, and Python runtime dependencies..."
apt-get install -y \
    python3 \
    python3-pip \
    python3-flask \
    python3-serial \
    python3-gpiozero \
    python3-picamera2 \
    python3-libcamera \
    libcamera-apps \
    gunicorn

# Ensure Python requirements are met
python3 -m pip install --break-system-packages pyserial flask gpiozero || true

# 3. Add User to Required Hardware Groups
echo "[+] Adding $TARGET_USER to hardware groups (gpio, dialout, video)..."
usermod -aG dialout,gpio,video "$TARGET_USER"

# 4. Hardware UART Configuration
echo "[+] Configuring Hardware UART on GPIO 14/15..."

# Detect boot config path (Bookworm vs legacy OS)
if [ -f "/boot/firmware/config.txt" ]; then
    CONFIG_FILE="/boot/firmware/config.txt"
    CMDLINE_FILE="/boot/firmware/cmdline.txt"
else
    CONFIG_FILE="/boot/config.txt"
    CMDLINE_FILE="/boot/cmdline.txt"
fi

# Ensure enable_uart=1 in config.txt
if ! grep -q "^enable_uart=1" "$CONFIG_FILE"; then
    echo "enable_uart=1" >> "$CONFIG_FILE"
    echo "[+] Enabled enable_uart=1 in $CONFIG_FILE"
fi

# Ensure camera auto detect is enabled
if ! grep -q "^camera_auto_detect=1" "$CONFIG_FILE"; then
    echo "camera_auto_detect=1" >> "$CONFIG_FILE"
    echo "[+] Enabled camera_auto_detect=1 in $CONFIG_FILE"
fi

# Disable serial login console from cmdline.txt to free /dev/serial0 for ESP32
if [ -f "$CMDLINE_FILE" ]; then
    sed -i 's/console=serial0,[0-9]* //g' "$CMDLINE_FILE"
    sed -i 's/console=ttyAMA0,[0-9]* //g' "$CMDLINE_FILE"
    echo "[+] Removed serial console binding from $CMDLINE_FILE"
fi

# Disable serial-getty service on serial port
systemctl stop serial-getty@serial0.service 2>/dev/null || true
systemctl disable serial-getty@serial0.service 2>/dev/null || true
systemctl stop serial-getty@ttyAMA0.service 2>/dev/null || true
systemctl disable serial-getty@ttyAMA0.service 2>/dev/null || true

# 5. Allow Passwordless Poweroff for Web UI / Inactivity Watchdog
echo "[+] Configuring sudoers for passwordless poweroff..."
cat <<EOF > /etc/sudoers.d/010_uv_chamber_poweroff
$TARGET_USER ALL=(ALL) NOPASSWD: /sbin/poweroff, /sbin/shutdown, /bin/systemctl poweroff
EOF
chmod 0440 /etc/sudoers.d/010_uv_chamber_poweroff

# 6. Ensure Storage Directory Exists
mkdir -p "$PROJECT_DIR/img"
chown -R "$TARGET_USER:$TARGET_USER" "$PROJECT_DIR/img"

# 7. Create and Enable Systemd Auto-Start Service
SERVICE_FILE="/etc/systemd/system/uv_chamber.service"
echo "[+] Creating systemd autostart service at $SERVICE_FILE..."

cat <<EOF > "$SERVICE_FILE"
[Unit]
Description=UV Chamber Camera Server & Battery Telemetry Monitor
After=network.target multi-user.target
Wants=network.target

[Service]
Type=simple
User=$TARGET_USER
WorkingDirectory=$PROJECT_DIR
ExecStart=/usr/bin/python3 $PROJECT_DIR/final.py
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal
Environment="PYTHONUNBUFFERED=1"

[Install]
WantedBy=multi-user.target
EOF

# Reload and enable the service
systemctl daemon-reload
systemctl enable uv_chamber.service
echo "[+] uv_chamber.service enabled on boot!"

# 8. Install Systemd Final Shutdown Hook for ESP32 Handshake
echo "[+] Installing systemd final shutdown hook (/lib/systemd/system-shutdown/esp32_notify.sh)..."
mkdir -p /lib/systemd/system-shutdown
cat << 'EOF' > /lib/systemd/system-shutdown/esp32_notify.sh
#!/bin/sh
# Runs at the absolute final stage of Linux shutdown after all filesystems are remounted read-only
if [ "$1" = "poweroff" ] || [ "$1" = "halt" ]; then
    stty -F /dev/serial0 9600 2>/dev/null || true
    printf "RPI:HALTED\n" > /dev/serial0 2>/dev/null || true
fi
EOF
chmod +x /lib/systemd/system-shutdown/esp32_notify.sh
echo "[+] esp32_notify.sh installed!"

# 9. Configure Wi-Fi Hotspot (Pi broadcasts its own network instead of joining one)
HOTSPOT_SSID="uvbox"
HOTSPOT_PASSWORD="uvbox@123"
HOTSPOT_CON="uvbox-hotspot"
HOTSPOT_IP="10.42.0.1"

echo "[+] Configuring Wi-Fi hotspot (SSID: $HOTSPOT_SSID)..."

if ! command -v nmcli >/dev/null 2>&1; then
    echo "[-] NetworkManager (nmcli) not found. Hotspot setup requires Raspberry Pi OS Bookworm or newer."
    exit 1
fi

# dnsmasq-base provides DHCP for clients that join the hotspot
apt-get install -y dnsmasq-base
rfkill unblock wlan 2>/dev/null || true

# Replace any previous hotspot profile so re-running the installer is safe
nmcli connection delete "$HOTSPOT_CON" 2>/dev/null || true

nmcli connection add type wifi ifname wlan0 con-name "$HOTSPOT_CON" ssid "$HOTSPOT_SSID" \
    autoconnect yes connection.autoconnect-priority 100 \
    802-11-wireless.mode ap 802-11-wireless.band bg 802-11-wireless.channel 6 \
    wifi-sec.key-mgmt wpa-psk wifi-sec.proto rsn \
    wifi-sec.pairwise ccmp wifi-sec.group ccmp wifi-sec.pmf disable \
    wifi-sec.psk "$HOTSPOT_PASSWORD" \
    ipv4.method shared ipv4.addresses "$HOTSPOT_IP/24" \
    ipv6.method disabled

# Stop the Pi from auto-joining saved Wi-Fi networks so wlan0 stays in hotspot mode.
# The hotspot is not activated now, to avoid dropping an SSH session over Wi-Fi mid-install;
# it comes up on the next boot.
HOTSPOT_UUID="$(nmcli -g connection.uuid connection show "$HOTSPOT_CON")"
nmcli -t -f UUID,TYPE connection show \
    | awk -F: -v skip="$HOTSPOT_UUID" '$2 == "802-11-wireless" && $1 != skip {print $1}' \
    | while read -r uuid; do
        nmcli connection modify "$uuid" connection.autoconnect no
    done
echo "[+] Hotspot '$HOTSPOT_SSID' configured (activates on next boot)!"

echo "=========================================================="
echo " Installation Complete!"
echo "----------------------------------------------------------"
echo " 1. Start the service now:"
echo "    sudo systemctl start uv_chamber.service"
echo ""
echo " 2. View live logs:"
echo "    sudo journalctl -u uv_chamber.service -f"
echo ""
echo " 3. Check web interface (after joining the '$HOTSPOT_SSID' Wi-Fi):"
echo "    http://$HOTSPOT_IP:5000/video_feed"
echo "    http://$HOTSPOT_IP:5000/battery"
echo ""
echo " Wi-Fi hotspot (active after reboot):"
echo "    SSID:     $HOTSPOT_SSID"
echo "    Password: $HOTSPOT_PASSWORD"
echo "    Pi IP:    $HOTSPOT_IP  (SSH: ssh $TARGET_USER@$HOTSPOT_IP)"
echo ""
echo " NOTE: A reboot is required to apply UART, group & hotspot changes."
echo "       After reboot the Pi will no longer join your Wi-Fi network."
echo "       sudo reboot"
echo "=========================================================="
