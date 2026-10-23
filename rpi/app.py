import io
import os
import time
import logging
import threading
from datetime import datetime, timedelta
from signal import pause
from subprocess import check_call

import serial
from flask import Flask, Response, send_from_directory, request, jsonify
from gpiozero import Button, LED
from libcamera import controls, Transform
from picamera2 import Picamera2
from picamera2.encoders import JpegEncoder
from picamera2.outputs import FileOutput

# --- Logging Setup ---
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

# --- Flask App ---
app = Flask(__name__)
IMAGE_DIRECTORY = "img"
os.makedirs(IMAGE_DIRECTORY, exist_ok=True)

# --- Streaming Output Buffer ---
class StreamingOutput(io.BufferedIOBase):
    def __init__(self):
        self.frame = None
        self.condition = threading.Condition()

    def write(self, buf):
        with self.condition:
            self.frame = buf
            self.condition.notify_all()

# --- Global State & Locks ---
state_lock = threading.Lock()
blink_lock = threading.Lock()
capture_in_progress = False

timer = time.time()
client_status = {'last_ping': None, 'status': False}

# Global Battery Telemetry from ESP32
battery_data = {
    'voltage': 0.0,
    'soc': 0,
    'last_updated': None,
    'connected': False
}

# --- Camera Initialization ---
try:
    camera = Picamera2()
    transform = Transform(rotation=90)

    config = camera.create_preview_configuration(
        main={"format": "XRGB8888", "size": (720, 1280)},  # High-res capture
        lores={"format": "YUV420", "size": (480, 854)},     # Low-res live stream
        transform=transform,
        buffer_count=4
    )
    camera.configure(config)

    streaming_output = StreamingOutput()
    encoder = JpegEncoder(q=70)

    camera.start_recording(encoder, FileOutput(streaming_output), name="lores")
    camera.set_controls({"AfMode": controls.AfModeEnum.Continuous})
    logging.info("Camera initialized and streaming encoder started.")
except Exception as e:
    logging.error(f"Failed to initialize camera: {e}")
    streaming_output = None

# --- Hardware Setup ---
led1_button = Button(17)
led2_button = Button(21)
capture_button = Button(4, hold_time=2)

led1 = LED(18, active_high=False)
led2 = LED(23, active_high=False)
# NOTE: Moved from GPIO 15 to GPIO 24 to prevent conflict with UART0 RX (GPIO 15)
ledc = LED(24)

# Initial LED state
led1.on()
led2.on()

# --- ESP32 UART Telemetry Thread ---
def esp32_uart_listener():
    """Reads telemetry strings from ESP32 over /dev/serial0 (GPIO 14 TX, GPIO 15 RX)."""
    global battery_data
    try:
        ser = serial.Serial('/dev/serial0', 9600, timeout=2)
        logging.info("Connected to ESP32 UART on /dev/serial0")
    except Exception as e:
        logging.error(f"Failed to open UART to ESP32: {e}")
        return

    while True:
        try:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
            # Expected format: "VOLT:12.35,SOC:89%"
            if "VOLT:" in line and "SOC:" in line:
                parts = line.split(',')
                volt_str = parts[0].replace("VOLT:", "").strip()
                soc_str = parts[1].replace("SOC:", "").replace("%", "").strip()
                
                with state_lock:
                    battery_data['voltage'] = float(volt_str)
                    battery_data['soc'] = int(soc_str)
                    battery_data['last_updated'] = datetime.now().isoformat()
                    battery_data['connected'] = True
        except Exception as e:
            logging.debug(f"UART read error: {e}")
            time.sleep(1)

# --- Helper Functions ---
def update_timer():
    global timer
    with state_lock:
        timer = time.time()

def blink():
    def blink_led():
        if not blink_lock.acquire(blocking=False):
            return
        try:
            for _ in range(6):
                ledc.on()
                time.sleep(0.2)
                ledc.off()
                time.sleep(0.2)
        finally:
            blink_lock.release()

    threading.Thread(target=blink_led, daemon=True).start()

def monitor_client():
    timeout_duration = timedelta(seconds=60)
    while True:
        time.sleep(25)
        with state_lock:
            if client_status['last_ping']:
                if datetime.now() - client_status['last_ping'] > timeout_duration:
                    client_status['status'] = False
                    logging.info("Client timeout detected.")

def shutdown_monitor():
    global timer
    idle_mode_active = False

    while True:
        time.sleep(5)
        with state_lock:
            current_time = time.time()
            duration = current_time - timer
            is_client_active = client_status["status"]

        if duration > 900 and not is_client_active:
            logging.warning("Inactivity shutdown triggered.")
            for _ in range(5):
                led1.toggle()
                led2.toggle()
                time.sleep(0.2)
            check_call(['sudo', 'poweroff'])

        if duration > 300:
            if not idle_mode_active:
                logging.info("Entering idle mode: Turning off LEDs.")
                if led1.is_active:
                    led1.off()
                if led2.is_active:
                    led2.off()
                idle_mode_active = True
        else:
            idle_mode_active = False

def capture_image():
    global capture_in_progress
    update_timer()
    ledc.on()

    timestamp = datetime.now().strftime("%Y-%m-%dT%H_%M_%S")
    filename = f"RF_pic_{timestamp}.jpeg"
    path = os.path.join(IMAGE_DIRECTORY, filename)

    try:
        logging.info("Starting autofocus cycle for capture...")
        success = camera.autofocus_cycle()
        if not success:
            logging.warning("Autofocus cycle failed or was skipped.")
        else:
            logging.info("Autofocus complete.")

        camera.capture_file(path)
        logging.info(f"Captured: {path}")
    except Exception as e:
        logging.error(f"Capture failed: {e}")
    finally:
        time.sleep(0.3)
        ledc.off()
        with state_lock:
            capture_in_progress = False

# --- Flask Routes ---
@app.route('/ping')
def ping():
    with state_lock:
        client_status['last_ping'] = datetime.now()
        client_status['status'] = True
    blink()
    update_timer()
    return jsonify(status="pong", code=200)

@app.route('/device_status')
def device_status():
    return jsonify(online=True)

@app.route('/battery')
def battery_route():
    """Returns real-time battery voltage and SOC from the ESP32."""
    with state_lock:
        return jsonify(battery_data)

@app.route('/capture', methods=['GET', 'POST'])
def trigger_capture():
    global capture_in_progress
    with state_lock:
        if capture_in_progress:
            return jsonify(status="busy"), 429
        capture_in_progress = True
        client_status['last_ping'] = datetime.now()

    threading.Thread(target=capture_image, daemon=True).start()
    return jsonify(status="capture_started")

@app.route('/list_files', methods=['GET'])
def list_files():
    try:
        page = int(request.args.get('page', 1))
        per_page = int(request.args.get('per_page', 10))

        files_with_stats = []
        with os.scandir(IMAGE_DIRECTORY) as entries:
            for entry in entries:
                if entry.is_file() and entry.name.lower().endswith(('.png', '.jpg', '.jpeg', '.gif')):
                    files_with_stats.append((entry.name, entry.stat().st_mtime))

        files_with_stats.sort(key=lambda x: x[1], reverse=True)
        sorted_filenames = [f[0] for f in files_with_stats]

        total_files = len(sorted_filenames)
        start = (page - 1) * per_page
        end = start + per_page
        paginated_files = sorted_filenames[start:end]

        return jsonify({
            "page": page,
            "per_page": per_page,
            "total": total_files,
            "files": paginated_files
        })
    except Exception as e:
        logging.error(f"Error listing files: {e}")
        return jsonify(error=str(e)), 500

@app.route('/images/<path:filename>')
def get_file(filename):
    return send_from_directory(IMAGE_DIRECTORY, filename)

@app.route('/led1_status')
def led1_status_route():
    return jsonify(active=led1.is_active)

@app.route('/led2_status')
def led2_status_route():
    return jsonify(active=led2.is_active)

@app.route('/uv_status')
def uv_status_route():
    return jsonify(UV_A=led1.is_active, UV_B=led2.is_active)

@app.route('/led1_toggle')
def toggle_led1_route():
    update_timer()
    led1.toggle()
    return jsonify(active=led1.is_active)

@app.route('/led2_toggle')
def toggle_led2_route():
    update_timer()
    led2.toggle()
    return jsonify(active=led2.is_active)

@app.route('/poweroff')
def poweroff_route():
    threading.Timer(1.0, lambda: check_call(['sudo', 'poweroff'])).start()
    return jsonify(status="powering_off")

# --- Video Streaming ---
def generate_frames():
    if streaming_output is None:
        return

    while True:
        with streaming_output.condition:
            streaming_output.condition.wait()
            frame = streaming_output.frame

        if frame is None:
            continue

        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame + b'\r\n')

@app.route('/video_feed')
def video_feed():
    return Response(generate_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

# --- Hardware Button Listener ---
def hardware_button_listener():
    led1_button.when_pressed = lambda: (update_timer(), led1.toggle())
    led2_button.when_pressed = lambda: (update_timer(), led2.toggle())

    capture_button.when_held = lambda: threading.Thread(target=capture_image, daemon=True).start()
    capture_button.when_pressed = update_timer

    pause()

# --- Main Entry Point ---
if __name__ == '__main__':
    threading.Thread(target=hardware_button_listener, daemon=True).start()
    threading.Thread(target=shutdown_monitor, daemon=True).start()
    threading.Thread(target=monitor_client, daemon=True).start()
    threading.Thread(target=esp32_uart_listener, daemon=True).start()

    app.run(host='0.0.0.0', port=5000, threaded=True)
