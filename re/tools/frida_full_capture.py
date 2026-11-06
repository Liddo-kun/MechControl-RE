"""
frida_full_capture.py - Frida capture for GCUService.exe

Hooks EC writes, USB HID, DDC/CI, WMI detection.
Captured calls are timestamped and sandwiched between action markers.

Modes:
  python frida_full_capture.py            - interactive (type labels freely)
  python frida_full_capture.py --ble      - interactive, console shows BLE events only
  python frida_full_capture.py --scripted - guided step-by-step fan curve capture

(must run as Administrator from cmd.exe or Windows Terminal)
"""

import frida
import sys
import os
import subprocess
import time
import threading
import msvcrt
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
JS_PATH = os.path.join(SCRIPT_DIR, 'frida_full_hook.js')
LOG_PATH = os.path.join(SCRIPT_DIR, 'frida_full_capture.log')


# ----------------------------------------------------------------------
# Scripted scenario: MechControl fan curve investigation on X6FR5xxY
# Used when --scripted is passed. Each step prints instructions, opens a
# marker when the user presses Enter, closes the marker on second Enter,
# then advances automatically to the next step.
# ----------------------------------------------------------------------
SCRIPTED_STEPS = [
    {
        "label": "baseline idle",
        "desc": (
            "Wait 10 seconds doing nothing, then press Enter.\n"
            "(captures what the OEM service does at rest)"
        ),
    },
    {
        "label": "mech break",
        "desc": (
            "Open ANOTHER cmd.exe window as Administrator, cd to the\n"
            "folder containing fan_curve_probe.exe, then run:\n"
            "    fan_curve_probe.exe 1\n"
            "Wait for it to finish (about 25 seconds), then come back\n"
            "here and press Enter.\n"
            "\n"
            "REQUIRES fan_curve_probe.exe on this machine. If you do\n"
            "not have it, type 'quit' now and ask Jon to send it."
        ),
    },
    {
        "label": "wait broken 10s",
        "desc": (
            "Wait 10 seconds with the broken curve live, then press Enter.\n"
            "(captures polling behavior while the curve is in a bad state)"
        ),
    },
    {
        "label": "open oem app",
        "desc": (
            "Launch the OEM Control Center UI now. Wait until the UI is\n"
            "fully loaded, then press Enter."
        ),
    },
    {
        "label": "switch to custom mode",
        "desc": (
            "In the OEM app, click the Custom / Manual fan mode option.\n"
            "Then press Enter."
        ),
    },
    {
        "label": "edit cpu point to 90",
        "desc": (
            "In the OEM app, drag ONE CPU fan curve point to 90%.\n"
            "(pick a mid-temp point around 60C - any clear value)\n"
            "Then press Enter."
        ),
    },
    {
        "label": "apply curve 90",
        "desc": (
            "Click the Apply button if there is one. Some OEM apps\n"
            "auto-apply when you release the mouse.\n"
            "Then press Enter."
        ),
    },
    {
        "label": "wait after apply 90",
        "desc": (
            "Wait 10 seconds. You should hear fans change.\n"
            "Then press Enter."
        ),
    },
    {
        "label": "edit cpu point to 30",
        "desc": (
            "In the OEM app, change the SAME point to 30%.\n"
            "Then press Enter."
        ),
    },
    {
        "label": "apply curve 30",
        "desc": (
            "Click Apply (or release the mouse if auto-apply).\n"
            "Then press Enter."
        ),
    },
    {
        "label": "wait after apply 30",
        "desc": (
            "Wait 10 seconds, then press Enter.\n"
            "This is the last step - after this the capture will finish."
        ),
    },
]


def read_line(prompt='> '):
    """Read a line using msvcrt — works even when Frida has hijacked stdin."""
    sys.stdout.write(prompt)
    sys.stdout.flush()
    chars = []
    while True:
        ch = msvcrt.getwch()
        if ch in ('\r', '\n'):
            sys.stdout.write('\n')
            sys.stdout.flush()
            return ''.join(chars)
        elif ch == '\x08':          # Backspace
            if chars:
                chars.pop()
                sys.stdout.write('\x08 \x08')
                sys.stdout.flush()
        elif ch == '\x03':          # Ctrl+C
            raise KeyboardInterrupt
        elif ch == '\x1b':          # Escape — ignore
            pass
        elif ch == '\x00' or ch == '\xe0':
            msvcrt.getwch()         # consume second byte of special key
        else:
            chars.append(ch)
            sys.stdout.write(ch)
            sys.stdout.flush()


class Capture:
    def __init__(self):
        self.logfile = None
        self.script = None
        self.session = None
        self.show_all_ioctl = False
        self.show_ec_reads = False
        self.show_poll = False
        self.ble_only = False
        self.recording = False
        self.print_lock = threading.Lock()

    def log(self, line):
        if self.logfile:
            self.logfile.write(line + '\n')
            self.logfile.flush()

    def cout(self, text):
        with self.print_lock:
            sys.stdout.write(text + '\n')
            sys.stdout.flush()

    def on_message(self, msg, data):
        if msg['type'] == 'send':
            payload = msg['payload']
            if isinstance(payload, dict):
                cat = payload.get('cat', 'setup')
                text = payload.get('text', '')
            else:
                cat = 'setup'
                text = str(payload)

            self.log(text)

            show = False
            if self.ble_only:
                # --ble mode: only show setup and BLE events on console
                show = cat in ('setup', 'ble')
            elif cat == 'setup':
                show = True
            elif cat == 'ec':
                if 'ECWRITE' in text or 'CUSTOMCTL' in text:
                    show = True
                elif 'ECREAD' in text and self.show_ec_reads:
                    show = True
            elif cat == 'ec_poll':
                show = self.show_poll
            elif cat in ('hid', 'ddc', 'wmi', 'file', 'ble'):
                show = True
            elif cat == 'ioctl':
                show = self.show_all_ioctl

            if show:
                if 'ECWRITE' in text:
                    self.cout('\n' + '=' * 70 +
                              '\n*** ' + text + ' ***\n' +
                              '=' * 70)
                elif cat == 'ble':
                    self.cout('\n>>> [BLE] ' + text)
                elif cat == 'hid':
                    self.cout('[HID] ' + text)
                elif cat == 'ddc':
                    self.cout('[DDC] ' + text)
                elif cat == 'wmi':
                    self.cout('[WMI] ' + text)
                else:
                    self.cout(text)

        elif msg['type'] == 'error':
            err = msg.get('description', str(msg))
            self.log('[FRIDA_ERROR] ' + err)
            self.cout('[ERROR] ' + err)

    def marker(self, action):
        ts = datetime.now().strftime('%H:%M:%S.%f')[:-3]
        sep = '#' * 70
        block = '\n' + sep + '\n### [ACTION: "' + action + '" at ' + ts + ']\n' + sep
        self.log(block)
        self.cout(block)
        self.recording = True

    def end_marker(self):
        ts = datetime.now().strftime('%H:%M:%S.%f')[:-3]
        line = '--- [END at ' + ts + '] ---'
        self.log('\n' + line)
        self.cout('\n' + line)
        self.recording = False

    def find_pid(self):
        # --ble mode targets the UI app (does BLE); default targets GCUService (does EC/HID)
        target = 'GamingCenter3_Cross' if self.ble_only else 'GCUService'
        result = subprocess.run(
            ['powershell', '-Command', '(Get-Process -Name ' + target + ').Id'],
            capture_output=True, text=True
        )
        if result.returncode != 0 or not result.stdout.strip():
            print('ERROR: ' + target + '.exe not running.')
            if self.ble_only:
                print('Open the OEM Control Center UI first.')
            else:
                print('Start the OEM Control Center service first.')
            sys.exit(1)
        return int(result.stdout.strip().split('\n')[0])

    def run(self):
        scripted = '--scripted' in sys.argv
        self.ble_only = '--ble' in sys.argv

        pid = self.find_pid()
        print('GCUService PID: ' + str(pid))

        with open(JS_PATH, 'r', encoding='utf-8') as f:
            js_code = f.read()

        self.logfile = open(LOG_PATH, 'w', encoding='utf-8')
        self.log('# Frida Full Capture')
        self.log('# Date: ' + datetime.now().strftime('%Y-%m-%d %H:%M:%S'))
        self.log('# PID: ' + str(pid))
        self.log('# Mode: ' + ('scripted' if scripted else 'interactive'))
        self.log('')

        print('Attaching to PID ' + str(pid) + '...')
        self.session = frida.attach(pid)
        self.script = self.session.create_script(js_code)
        self.script.on('message', self.on_message)
        self.script.load()

        time.sleep(1.5)

        try:
            if scripted:
                self._run_scripted()
            else:
                self._run_interactive()
        except KeyboardInterrupt:
            print('\nInterrupted.')
            if self.recording:
                self.end_marker()

        print('\nDetaching...')
        try:
            self.session.detach()
        except Exception:
            pass
        self.logfile.close()
        print('Log saved: ' + LOG_PATH)

    def _run_interactive(self):
        mode_label = 'BLE ONLY' if self.ble_only else 'interactive'
        print('')
        print('=' * 70)
        print('  CAPTURE ACTIVE (' + mode_label + ' mode)')
        if self.ble_only:
            print('  Console shows: BLE events only')
        else:
            print('  Console shows: EC WRITES, HID, DDC/CI, WMI, BLE')
        print('  Everything saved to log file')
        print('=' * 70)
        print('')
        print('  Describe what you will do, press ENTER to mark it.')
        print('  Do the action in OEM app. Press ENTER when done.')
        print('  Type "done" to quit.')
        print('')
        print('  /reads  toggle EC reads    /all  toggle all IOCTLs')
        print('  /poll   toggle poll reads   /mods list modules')
        print('')
        print('Log: ' + LOG_PATH)
        print('')

        while True:
            action = read_line('> ').strip()

            if not action:
                if self.recording:
                    self.end_marker()
                    print('')
                continue

            low = action.lower()

            if low == 'done':
                if self.recording:
                    self.end_marker()
                break
            elif low == '/reads':
                self.show_ec_reads = not self.show_ec_reads
                print('[CONFIG] EC reads: ' +
                      ('ON' if self.show_ec_reads else 'OFF'))
            elif low == '/all':
                self.show_all_ioctl = not self.show_all_ioctl
                try:
                    self.script.exports_sync.set_ioctl_mode(self.show_all_ioctl)
                except Exception:
                    pass
                print('[CONFIG] All IOCTLs: ' +
                      ('ON' if self.show_all_ioctl else 'OFF'))
            elif low == '/poll':
                self.show_poll = not self.show_poll
                print('[CONFIG] Polling reads: ' +
                      ('ON' if self.show_poll else 'OFF'))
            elif low == '/mods':
                try:
                    mods = self.script.exports_sync.get_modules()
                    print('Loaded (' + str(len(mods)) + '):')
                    for m in mods:
                        print('  ' + m)
                except Exception as e:
                    print('Error: ' + str(e))
            else:
                if self.recording:
                    self.end_marker()
                self.marker(action)
                print('  >>> Do it now. Press ENTER when done.')

    def _run_scripted(self):
        total = len(SCRIPTED_STEPS)
        print('')
        print('=' * 70)
        print('  SCRIPTED CAPTURE - ' + str(total) + ' steps')
        print('=' * 70)
        print('')
        print('For every step:')
        print('  1. Read the instructions')
        print('  2. Press Enter to START the step   (marker opens)')
        print('  3. Do the action')
        print('  4. Press Enter to END the step     (marker closes)')
        print('')
        print('Type "quit" at any prompt to abort early.')
        print('')
        print('Log: ' + LOG_PATH)
        print('')
        resp = read_line('Press Enter when ready to begin (or "quit"): ').strip().lower()
        if resp == 'quit':
            print('Aborted before first step.')
            return

        for i, step in enumerate(SCRIPTED_STEPS, 1):
            print('')
            print('=' * 70)
            print('  STEP ' + str(i) + ' of ' + str(total) + ':  ' + step['label'])
            print('=' * 70)
            print(step['desc'])
            print('')
            resp = read_line('>>> Press Enter to START (or "quit"): ').strip().lower()
            if resp == 'quit':
                print('Aborted before step ' + str(i) + '.')
                return

            self.marker(step['label'])
            print('')
            print('  [MARKER OPEN - do the action now]')
            resp = read_line('>>> Press Enter when DONE (or "quit"): ').strip().lower()
            self.end_marker()
            if resp == 'quit':
                print('Aborted after step ' + str(i) + '.')
                return

        print('')
        print('=' * 70)
        print('  ALL ' + str(total) + ' STEPS COMPLETE')
        print('=' * 70)


if __name__ == '__main__':
    Capture().run()
