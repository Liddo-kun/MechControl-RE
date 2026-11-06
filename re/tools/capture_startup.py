"""
Capture GCUService startup: start GCUBridge service, attach Frida to
GCUService.exe ASAP, log all EC/HID/WMI calls to file.
Run as Administrator.
"""

import subprocess
import time
import sys
import os
import json
import frida

HOOK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "frida_full_hook.js")
LOG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "frida_startup_log.txt")

def find_pid(name):
    """Find PID of a process by name using tasklist."""
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"],
            text=True, stderr=subprocess.DEVNULL
        )
        for line in out.strip().split("\n"):
            if name.lower() in line.lower():
                parts = line.strip().strip('"').split('","')
                if len(parts) >= 2:
                    return int(parts[1].strip('"'))
    except Exception:
        pass
    return None

def main():
    # Check GCUService isn't already running
    pid = find_pid("GCUService.exe")
    if pid:
        print(f"[!] GCUService.exe already running (PID {pid}). Stop service first.")
        sys.exit(1)

    # Load hook script
    with open(HOOK_SCRIPT, "r") as f:
        hook_code = f.read()

    # Open log file
    log_f = open(LOG_FILE, "w", encoding="utf-8")
    t0 = time.time()

    def log(msg):
        elapsed = time.time() - t0
        line = f"[+{elapsed:07.3f}s] {msg}"
        print(line)
        log_f.write(line + "\n")
        log_f.flush()

    log("Starting GCUBridge service...")

    # Start service (non-blocking — Start-Service returns when service is "Running"
    # but GCUService.exe may already be doing init by then)
    proc = subprocess.Popen(
        ["powershell.exe", "-Command", "Start-Service GCUBridge"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )

    # Poll for GCUService.exe — check every 50ms
    log("Polling for GCUService.exe process...")
    poll_start = time.time()
    pid = None
    while time.time() - poll_start < 30:
        pid = find_pid("GCUService.exe")
        if pid:
            break
        time.sleep(0.05)

    if not pid:
        log("FAILED: GCUService.exe did not appear within 30s")
        # Check if service started at all
        proc.wait()
        stdout, stderr = proc.communicate()
        log(f"Service start stdout: {stdout.decode(errors='replace')}")
        log(f"Service start stderr: {stderr.decode(errors='replace')}")
        log_f.close()
        sys.exit(1)

    attach_delay = time.time() - t0
    log(f"Found GCUService.exe PID={pid} (after {attach_delay:.3f}s)")

    # Attach Frida
    log("Attaching Frida...")
    try:
        session = frida.attach(pid)
    except Exception as e:
        log(f"FAILED to attach Frida: {e}")
        log_f.close()
        sys.exit(1)

    frida_delay = time.time() - t0
    log(f"Frida attached (after {frida_delay:.3f}s)")

    # Message handler
    msg_count = [0]

    def on_message(message, data):
        msg_count[0] += 1
        elapsed = time.time() - t0
        if message["type"] == "send":
            payload = message["payload"]
            if isinstance(payload, dict):
                cat = payload.get("cat", "?")
                text = payload.get("text", "")
                line = f"[+{elapsed:07.3f}s] [{cat}] {text}"
            else:
                line = f"[+{elapsed:07.3f}s] {payload}"
        elif message["type"] == "error":
            line = f"[+{elapsed:07.3f}s] [ERROR] {message.get('description', message)}"
        else:
            line = f"[+{elapsed:07.3f}s] [MSG] {message}"

        print(line)
        log_f.write(line + "\n")
        log_f.flush()

    script = session.create_script(hook_code)
    script.on("message", on_message)

    log("Loading hook script...")
    script.load()
    log("Hooks installed! Capturing all calls...")
    log("")
    log("=" * 60)
    log("CAPTURING — press Ctrl+C to stop and save")
    log("=" * 60)
    log("")

    # Wait for service start to complete
    proc.wait()

    # Let it run — capture init + steady state
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        elapsed = time.time() - t0
        log("")
        log(f"Stopped after {elapsed:.1f}s, captured {msg_count[0]} messages")

    # Cleanup
    try:
        script.unload()
        session.detach()
    except Exception:
        pass

    log_f.close()
    print(f"\nLog saved to: {LOG_FILE}")

if __name__ == "__main__":
    main()
