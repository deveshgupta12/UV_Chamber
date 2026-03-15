from libcamera import controls, Transform
from gpiozero import Button, LED
import time
from datetime import datetime, timedelta
from signal import pause
from flask import Flask, Response, send_from_directory, request, jsonify
from picamera2 import Picamera2
import cv2
import threading
from subprocess import check_call
import os
import logging

# Setup basic logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

# Initialize Flask app
app = Flask(__name__)
IMAGE_DIRECTORY = "img/"

# Ensure image directory exists
os.makedirs(IMAGE_DIRECTORY, exist_ok=True)

# --- Global State & Locks ---
state_lock = threading.Lock()
camera_lock = threading.Lock()
blink_lock = threading.Lock()

timer = time.time()
client_status = {'last_ping': None, 'status': False}

# --- Hardware Setup ---
# Configure and start the camera
try:
    camera = Picamera2()
    # Reduced preview size slightly for better streaming performance, adjust if needed
    transform = Transform(rotation=90)
    camera.configure(camera.create_preview_configuration(
        main={"format": 'XRGB8888', "size": (720, 1280)},
        transform=transform
    ))
    camera.start()
    camera.set_controls({"AfMode": controls.AfModeEnum.Continuous})
except Exception as e:
    logging.error(f"Failed to initialize camera: {e}")
    # You might want to exit here depending on how critical the camera is on startup

# Define the buttons and LEDs
led1_button = Button(2)
led2_button = Button(3)
capture_button = Button(4, hold_time=2)
led1 = LED(18, active_high=False)
led2 = LED(23, active_high=False)
ledc = LED(15)

# Initial LED state
led1.on()
led2.on()

# --- Helper Functions ---

def update_timer():
    """Safely update the global inactivity timer."""
    global timer
    with state_lock:
        timer = time.time()

def blink():
    """Blinks the capture LED without overlapping threads."""
    def blink_led():
        # Try to acquire lock; if we can't, it means it's already blinking.
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
    """Periodically checks if the client has timed out."""
    timeout_duration = timedelta(seconds=60)
    while True:
        time.sleep(25)
        with state_lock:
            if client_status['last_ping']:
                if datetime.now() - client_status['last_ping'] > timeout_duration:
                    client_status['status'] = False
                    logging.info("Client timeout detected.")

def shutdown_monitor():
    """Handles automatic power-off and idle LED states."""
    global timer
    idle_mode_active = False

    while True:
        time.sleep(5)
        with state_lock:
            current_time = time.time()
            duration = current_time - timer
            is_client_active = client_status["status"]

        # Power off after 15 minutes of inactivity and no active client
        if duration > 900 and not is_client_active:
            logging.warning("Inactivity shutdown triggered.")
            # Blink LEDs rapidly before shutdown as a warning
            for _ in range(5):
                led1.toggle()
                led2.toggle()
                time.sleep(0.2)
            check_call(['sudo', 'poweroff'])

        # Enter idle mode after 300 seconds
        if duration > 300:
            if not idle_mode_active:
                logging.info("Entering idle mode: Turning off LEDs.")
                if led1.is_active: led1.off()
                if led2.is_active: led2.off()
                idle_mode_active = True
        else:
            # Reset idle flag if activity occurred recently
            idle_mode_active = False

def capture_image():
    """Captures a still image safely after running an autofocus cycle."""
    with state_lock:
        client_status['last_ping'] = datetime.now()
    
    update_timer()
    ledc.on()
    
    timestamp = datetime.now().strftime("%Y-%m-%dT%H_%M_%S")
    filename = f"RF_pic_{timestamp}.jpeg"
    path = os.path.join(IMAGE_DIRECTORY, filename)
    
    try:
        # Lock camera to prevent conflict with video stream
        with camera_lock:
            logging.info("Starting autofocus cycle for capture...")
            # Trigger a single, precise autofocus cycle. This is blocking.
            # This temporarily overrides the "Continuous" mode for the capture.
            success = camera.autofocus_cycle()
            if not success:
                logging.warning("Autofocus cycle failed or was skipped.")
            else:
                logging.info("Autofocus complete.")

            # Capture the file after focus is set
            camera.capture_file(path)
            
        logging.info(f"Captured: {path}")
        
    except Exception as e:
        logging.error(f"Capture failed: {e}")
    finally:
        time.sleep(0.5) # Keep short feedback blink
        ledc.off()

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

@app.route('/capture', methods=['GET', 'POST'])
def trigger_capture():
    # Run capture in a separate thread so we don't block the HTTP response immediately
    threading.Thread(target=capture_image, daemon=True).start()
    return jsonify(status="capture_started")

@app.route('/list_files', methods=['GET'])
def list_files():
    try:
        page = int(request.args.get('page', 1))
        per_page = int(request.args.get('per_page', 10))

        # Use scandir for better performance, get stats for sorting
        files_with_stats = []
        with os.scandir(IMAGE_DIRECTORY) as entries:
            for entry in entries:
                if entry.is_file() and entry.name.lower().endswith(('.png', '.jpg', '.jpeg', '.gif')):
                    files_with_stats.append((entry.name, entry.stat().st_mtime))

        # Sort by modification time, newest first
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
    # Delay poweroff slightly to allow response to be sent
    threading.Timer(1.0, lambda: check_call(['sudo', 'poweroff'])).start()
    return jsonify(status="powering_off")

# --- Video Streaming ---

def generate_frames():
    while True:
        # Use lock to ensure we don't try to stream while a high-res capture is happening
        with camera_lock:
            frame = camera.capture_array()
        
        if frame is None:
            time.sleep(0.1)
            continue

        # Encode frame to JPEG
        ret, buffer = cv2.imencode('.jpg', frame)
        if not ret:
            continue
            
        frame_bytes = buffer.tobytes()
        yield (b'--frame\r\n'
               b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
        
        # Optional: small sleep to cap framerate and reduce CPU load if needed
        # time.sleep(0.03) 

@app.route('/video_feed')
def video_feed():
    return Response(generate_frames(), mimetype='multipart/x-mixed-replace; boundary=frame')

# --- Main Entry Point ---

def hardware_button_listener():
    """Connects physical buttons to their actions."""
    # Using lambda wrappers to ensure they hit the update_timer correctly
    led1_button.when_pressed = lambda: (update_timer(), led1.toggle())
    led2_button.when_pressed = lambda: (update_timer(), led2.toggle())
    
    # Capture on hold
    capture_button.when_held = capture_image
    
    # Just update timer on simple press to keep device awake
    capture_button.when_pressed = update_timer 
    
    pause()

if __name__ == '__main__':
    # Start background threads as daemons so they die when main app dies
    threading.Thread(target=hardware_button_listener, daemon=True).start()
    threading.Thread(target=shutdown_monitor, daemon=True).start()
    threading.Thread(target=monitor_client, daemon=True).start()
    
    # Note: For true production, run this app with Gunicorn/Waitress, 
    # NOT app.run(). Example: gunicorn -w 1 -b 0.0.0.0:5000 --threads 4 app:app
    app.run(host='0.0.0.0', port=5000, threaded=True)