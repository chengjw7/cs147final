from flask import Flask, render_template, jsonify, request
import requests
from datetime import datetime, timezone

app = Flask(__name__)

# ========== Your ThingSpeak configuration ==========
CHANNEL_ID = "3199845"
READ_API_KEY = "JKJQFNBEDDVENAAM"
WRITE_API_KEY = "A94QD484Q4O3IOLX"
TS_BASE = "https://api.thingspeak.com"

# ===== Helper: convert ISO time → readable local string =====
def pct_time(iso_str):
    """
    Convert ThingSpeak ISO time like '2025-12-11T05:42:33Z'
    into a local readable format '2025-12-11 13:42:33' (local time)
    """
    if not iso_str:
        return None
    try:
        dt = datetime.strptime(iso_str, "%Y-%m-%dT%H:%M:%SZ")
        # convert to local timezone (assuming UTC+8, change if needed)
        local_dt = dt.replace(tzinfo=timezone.utc).astimezone()
        return local_dt.strftime("%Y-%m-%d %H:%M:%S")
    except Exception:
        return iso_str

@app.route("/")
def home():
    """Serve the main dashboard"""
    return render_template("index.html")

@app.route("/api/latest")
def api_latest():
    """Return the latest sensor readings from ThingSpeak"""
    url = f"{TS_BASE}/channels/{CHANNEL_ID}/feeds.json"
    params = {"api_key": READ_API_KEY, "results": 1}
    r = requests.get(url, params=params, timeout=6)
    data = r.json()
    feeds = data.get("feeds", [])
    if not feeds:
        return jsonify({"error": "no_data"}), 200
    f = feeds[0]
    return jsonify({
        "temperature": f.get("field1"),
        "humidity": f.get("field2"),
        "ldr": f.get("field3"),
        "time": pct_time(f.get("created_at"))
    })

@app.route("/api/history")
def api_history():
    """Return the last 60 readings for chart display"""
    url = f"{TS_BASE}/channels/{CHANNEL_ID}/feeds.json"
    params = {"api_key": READ_API_KEY, "results": 60}
    r = requests.get(url, params=params, timeout=8)
    data = r.json()
    feeds = data.get("feeds", [])
    out = []
    for f in feeds:
        out.append({
            "t": pct_time(f.get("created_at")),
            "temp": try_float(f.get("field1")),
            "hum": try_float(f.get("field2")),
            "ldr": try_int(f.get("field3")),
        })
    return jsonify(out)

@app.route("/api/command", methods=["POST"])
def api_command():
    """Send a command (OPEN/CLOSE/AUTO) to ThingSpeak field4"""
    payload = request.get_json(silent=True) or {}
    cmd = (payload.get("cmd") or "").upper().strip()
    if cmd not in ("OPEN", "CLOSE", "AUTO"):
        return jsonify({"ok": False, "error": "bad_cmd"}), 400

    url = f"{TS_BASE}/update"
    params = {"api_key": WRITE_API_KEY, "field4": cmd}
    r = requests.get(url, params=params, timeout=5)
    ok = (r.status_code == 200 and r.text.strip().isdigit() and int(r.text) > 0)
    return jsonify({"ok": ok, "entry_id": r.text.strip()})

def try_float(x):
    try:
        return float(x)
    except Exception:
        return None

def try_int(x):
    try:
        return int(float(x))
    except Exception:
        return None

if __name__ == "__main__":
    app.run(debug=True)
