"""
frida_startup_capture.py - Targeted GCUService startup capture

Captures what GCUService does at startup to re-establish fan control
after MechControl's broken apply.

Key features:
  - Independent EC duty polling via ctypes (works without GCUService)
  - Fast PID detection via CreateToolhelp32Snapshot (~1ms per poll)
  - Auto-attach when GCUService appears
  - Verifies whether all EC calls were captured

Must run as Administrator.
"""

import frida
import sys
import os
import time
import threading
import ctypes
from ctypes import wintypes
import msvcrt
from datetime import datetime

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
JS_PATH = os.path.join(SCRIPT_DIR, 'frida_full_hook.js')
LOG_PATH = os.path.join(SCRIPT_DIR, 'frida_startup_capture.log')


# ======================================================================
# Windows API setup
# ======================================================================
kernel32 = ctypes.windll.kernel32

# CreateFileW
kernel32.CreateFileW.restype = ctypes.c_void_p
kernel32.CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
    wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE
]

# DeviceIoControl
kernel32.DeviceIoControl.restype = wintypes.BOOL
kernel32.DeviceIoControl.argtypes = [
    ctypes.c_void_p, wintypes.DWORD,
    wintypes.LPVOID, wintypes.DWORD,
    wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID
]

# CloseHandle
kernel32.CloseHandle.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

# Process enumeration
TH32CS_SNAPPROCESS = 0x00000002


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ('dwSize', wintypes.DWORD),
        ('cntUsage', wintypes.DWORD),
        ('th32ProcessID', wintypes.DWORD),
        ('th32DefaultHeapID', ctypes.c_size_t),
        ('th32ModuleID', wintypes.DWORD),
        ('cntThreads', wintypes.DWORD),
        ('th32ParentProcessID', wintypes.DWORD),
        ('pcPriClassBase', wintypes.LONG),
        ('dwFlags', wintypes.DWORD),
        ('szExeFile', ctypes.c_wchar * 260),
    ]


kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.Process32FirstW.restype = wintypes.BOOL
kernel32.Process32FirstW.argtypes = [ctypes.c_void_p,
                                     ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.restype = wintypes.BOOL
kernel32.Process32NextW.argtypes = [ctypes.c_void_p,
                                    ctypes.POINTER(PROCESSENTRY32W)]

# EC constants
IOCTL_ECREAD = 0x9C40A488
GENERIC_RW = 0xC0000000
SHARE_RW = 3
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


# ======================================================================
# Low-level helpers
# ======================================================================

def find_pid(name='GCUService.exe'):
    """Find PID by process name using native snapshot API (~1ms)."""
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap is None or snap == INVALID_HANDLE_VALUE:
        return None
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    try:
        if not kernel32.Process32FirstW(snap, ctypes.byref(entry)):
            return None
        while True:
            if entry.szExeFile.lower() == name.lower():
                return entry.th32ProcessID
            if not kernel32.Process32NextW(snap, ctypes.byref(entry)):
                return None
    finally:
        kernel32.CloseHandle(snap)


def ec_open():
    """Open ACPIDriver device handle. Returns handle or None."""
    h = kernel32.CreateFileW(
        "\\\\.\\ACPIDriver", GENERIC_RW, SHARE_RW,
        None, OPEN_EXISTING, 0, None
    )
    if h is None or h == INVALID_HANDLE_VALUE:
        return None
    return h


def ec_read(handle, addr):
    """Read one EC register byte. Returns int 0-255 or None on failure."""
    in_buf = wintypes.DWORD(addr)
    out_buf = wintypes.DWORD(0)
    ret = wintypes.DWORD(0)
    ok = kernel32.DeviceIoControl(
        handle, IOCTL_ECREAD,
        ctypes.byref(in_buf), 4,
        ctypes.byref(out_buf), 4,
        ctypes.byref(ret), None
    )
    return (out_buf.value & 0xFF) if ok else None


def read_line(prompt='> '):
    """Read a line using msvcrt -- works even when Frida has hijacked stdin."""
    sys.stdout.write(prompt)
    sys.stdout.flush()
    chars = []
    while True:
        ch = msvcrt.getwch()
        if ch in ('\r', '\n'):
            sys.stdout.write('\n')
            sys.stdout.flush()
            return ''.join(chars)
        elif ch == '\x08':
            if chars:
                chars.pop()
                sys.stdout.write('\x08 \x08')
                sys.stdout.flush()
        elif ch == '\x03':
            raise KeyboardInterrupt
        elif ch == '\x1b':
            pass
        elif ch in ('\x00', '\xe0'):
            msvcrt.getwch()
        else:
            chars.append(ch)
            sys.stdout.write(ch)
            sys.stdout.flush()


# ======================================================================
# Duty polling register set
# ======================================================================
# (addr, is_duty_raw_div_2)
POLL_REGS = [
    ('cpuDuty', 0x75B, True),
    ('gpuDuty', 0x75C, True),
    ('cpuTemp', 0x43E, False),
    ('gpuTemp', 0x44F, False),
    ('7C5',     0x7C5, False),
    ('7C6',     0x7C6, False),
    ('706',     0x706, False),
]


# ======================================================================
# Main capture class
# ======================================================================

class StartupCapture:
    def __init__(self):
        self.logfile = None
        self.frida_script = None
        self.frida_session = None
        self.ec_handle = None
        self.js_code = None
        self.print_lock = threading.Lock()
        self.duty_stop = threading.Event()
        self.duty_to_console = False
        self.attached = False

    # ---------- I/O helpers ----------

    def log(self, line):
        if self.logfile:
            self.logfile.write(line + '\n')
            self.logfile.flush()

    def cout(self, text):
        with self.print_lock:
            sys.stdout.write(text + '\n')
            sys.stdout.flush()

    # ---------- Frida message handler ----------

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
            if cat == 'setup':
                show = True
            elif cat == 'ec':
                # Show all EC activity -- this is a targeted capture
                show = True
            elif cat in ('hid', 'ddc', 'wmi', 'file'):
                show = True

            if show:
                if 'ECWRITE' in text:
                    self.cout('\n' + '=' * 70 +
                              '\n*** ' + text + ' ***\n' +
                              '=' * 70)
                elif cat == 'file':
                    self.cout('[FILE] ' + text)
                else:
                    self.cout(text)

        elif msg['type'] == 'error':
            err = msg.get('description', str(msg))
            self.log('[FRIDA_ERROR] ' + err)
            self.cout('[ERROR] ' + err)

    # ---------- Duty polling ----------

    def duty_poll_loop(self):
        """Background thread: read duty + control regs every 500ms."""
        while not self.duty_stop.is_set():
            vals = {}
            for name, addr, _ in POLL_REGS:
                vals[name] = ec_read(self.ec_handle, addr)

            t = datetime.now().strftime('%H:%M:%S.%f')[:-3]

            cd = vals.get('cpuDuty')
            gd = vals.get('gpuDuty')
            ct = vals.get('cpuTemp')
            gt = vals.get('gpuTemp')
            r5 = vals.get('7C5')
            r6 = vals.get('7C6')
            r0 = vals.get('706')

            if cd is not None:
                line = ('DUTY: [%s] CPU=%d%%(0x%02X) GPU=%d%%(0x%02X) '
                        'T=%s/%sC 7C5=0x%02X 7C6=0x%02X 706=0x%02X'
                        % (t, cd // 2, cd, gd // 2, gd,
                           ct, gt, r5, r6, r0))
            else:
                line = 'DUTY: [%s] READ FAILED' % t

            self.log(line)
            if self.duty_to_console:
                self.cout(line)
            self.duty_stop.wait(0.5)

    # ---------- Markers ----------

    def marker(self, label):
        t = datetime.now().strftime('%H:%M:%S.%f')[:-3]
        sep = '#' * 70
        block = '\n%s\n### [ACTION: "%s" at %s]\n%s' % (sep, label, t, sep)
        self.log(block)
        self.cout(block)

    def end_marker(self):
        t = datetime.now().strftime('%H:%M:%S.%f')[:-3]
        line = '--- [END at %s] ---' % t
        self.log('\n' + line)
        self.cout('\n' + line)

    # ---------- PID wait + Frida attach ----------

    def wait_and_attach(self):
        """Poll for GCUService PID every 20ms, attach Frida when found."""
        self.cout('[WAIT] Polling for GCUService.exe every 20ms...')
        self.log('[WAIT] Polling for GCUService.exe')

        poll_count = 0
        while not self.duty_stop.is_set():
            pid = find_pid('GCUService.exe')
            if pid:
                detect_time = datetime.now().strftime('%H:%M:%S.%f')[:-3]
                self.cout('')
                self.cout('[FOUND] GCUService.exe PID=%d at %s'
                          % (pid, detect_time))
                self.log('[FOUND] GCUService.exe PID=%d at %s'
                         % (pid, detect_time))

                # Brief pause for process init
                time.sleep(0.1)

                try:
                    self.frida_session = frida.attach(pid)
                    self.frida_script = self.frida_session.create_script(
                        self.js_code)
                    self.frida_script.on('message', self.on_message)
                    self.frida_script.load()
                    time.sleep(0.5)

                    attach_time = datetime.now().strftime('%H:%M:%S.%f')[:-3]
                    self.log('[ATTACHED] Frida hooks active at %s' % attach_time)
                    self.cout('')
                    self.cout('=' * 70)
                    self.cout('  [ATTACHED] Frida hooks active at %s'
                              % attach_time)
                    self.cout('=' * 70)

                    # Verify: did we catch ACPIDriver open?
                    self._verify_acpi_coverage()

                    self.attached = True
                    return True

                except frida.ProcessNotFoundError:
                    self.cout('[WARN] Process vanished during attach, '
                              'retrying...')
                except Exception as e:
                    self.cout('[ERROR] Frida attach failed: %s' % e)
                    return False

            poll_count += 1
            if poll_count % 500 == 0:
                self.cout('[WAIT] Still waiting... (%ds)'
                          % (poll_count * 20 // 1000))
            time.sleep(0.02)

        return False

    def _verify_acpi_coverage(self):
        """Check if CreateFileW hook caught ACPIDriver open."""
        try:
            acpi = self.frida_script.exports_sync.get_acpi_handles()
            if acpi:
                self.cout('[VERIFY] Caught ACPIDriver open: %s' % acpi)
                self.cout('[VERIFY] PROVABLY captured all EC calls.')
                self.log('[VERIFY] Caught ACPIDriver open: %s' % acpi)
                self.log('[VERIFY] PROVABLY captured all EC calls.')
            else:
                self.cout('[VERIFY] ACPIDriver not yet opened '
                          '(checking again in 2s...)')
                time.sleep(2)
                acpi = self.frida_script.exports_sync.get_acpi_handles()
                if acpi:
                    self.cout('[VERIFY] Caught ACPIDriver open: %s' % acpi)
                    self.cout('[VERIFY] PROVABLY captured all EC calls.')
                    self.log('[VERIFY] Caught ACPIDriver open: %s' % acpi)
                else:
                    self.cout('[WARN] ACPIDriver may have been opened '
                              'before attach.')
                    self.cout('[WARN] Some early EC calls might be missing.')
                    self.log('[WARN] ACPIDriver may have been opened '
                             'before Frida attach.')
        except Exception as e:
            self.cout('[WARN] Could not verify ACPIDriver: %s' % e)

    # ---------- Guided step helpers ----------

    def step_header(self, num, total, title):
        print('')
        print('=' * 70)
        print('  STEP %d of %d:  %s' % (num, total, title))
        print('=' * 70)

    def step_wait(self, prompt='Press Enter when done (or "quit"): '):
        resp = read_line('>>> ' + prompt).strip().lower()
        if resp == 'quit':
            return False
        return True

    # ---------- Main flow ----------

    def run(self):
        TOTAL_STEPS = 4

        # Pre-load JS
        with open(JS_PATH, 'r', encoding='utf-8') as f:
            self.js_code = f.read()

        # Open EC device for duty polling
        self.ec_handle = ec_open()
        if self.ec_handle is None:
            print('[ERROR] Cannot open \\\\.\\ACPIDriver')
            print('Run as Administrator.')
            sys.exit(1)

        # Open log
        self.logfile = open(LOG_PATH, 'w', encoding='utf-8')
        self.log('# Frida Startup Capture')
        self.log('# Date: ' + datetime.now().strftime('%Y-%m-%d %H:%M:%S'))
        self.log('# Purpose: Capture GCUService startup EC sequence')
        self.log('')

        print('')
        print('=' * 70)
        print('  TARGETED STARTUP CAPTURE')
        print('  Captures what GCUService does at startup to activate')
        print('  fan curves after MechControl\'s broken apply.')
        print('=' * 70)
        print('')
        print('This script will walk you through 4 steps.')
        print('Type "quit" at any prompt to abort.')
        print('')
        print('Log: ' + LOG_PATH)

        # Start duty polling immediately so user sees fan state throughout
        duty_thread = threading.Thread(target=self.duty_poll_loop,
                                       daemon=True)
        duty_thread.start()

        try:
            # ----------------------------------------------------------
            # STEP 1: Set up OEM app
            # ----------------------------------------------------------
            self.step_header(1, TOTAL_STEPS, 'Set up OEM app')
            print('')
            print('  1. Open the OEM Control Center app')
            print('  2. Set fan mode to Custom')
            print('  3. Set fans to 80%% flat (or any distinctive speed)')
            print('  4. Click Apply')
            print('  5. You should hear the fans speed up')
            print('')
            print('  (DUTY lines below show current fan speed)')
            print('')
            self.marker('step 1 - OEM setup')
            if not self.step_wait():
                self._cleanup(duty_thread)
                return
            self.end_marker()

            # ----------------------------------------------------------
            # STEP 2: Stop GCUService
            # ----------------------------------------------------------
            self.step_header(2, TOTAL_STEPS, 'Stop GCUService')
            print('')
            print('  Stop GCUService using ONE of these methods:')
            print('    - Services panel (services.msc) > GCUService > Stop')
            print('    - In another admin cmd:  net stop GCUService')
            print('')
            print('  Fans will likely drop to default speed as the')
            print('  service cleans up on shutdown. This is normal.')
            print('')
            self.marker('step 2 - stop GCUService')
            if not self.step_wait():
                self._cleanup(duty_thread)
                return
            self.end_marker()

            # Verify it's actually stopped
            pid = find_pid('GCUService.exe')
            if pid:
                print('')
                print('[WARN] GCUService.exe is still running (PID=%d)!'
                      % pid)
                print('       Please stop it before continuing.')
                print('')
                if not self.step_wait('Press Enter after stopping it '
                                      '(or "quit"): '):
                    self._cleanup(duty_thread)
                    return
                pid = find_pid('GCUService.exe')
                if pid:
                    print('[WARN] Still running. Continuing anyway...')

            # ----------------------------------------------------------
            # STEP 3: Break fans with MechControl
            # ----------------------------------------------------------
            self.step_header(3, TOTAL_STEPS, 'Break fans with MechControl')
            print('')
            print('  1. Open MechControl')
            print('  2. Enable custom fan curve')
            print('  3. Set BOTH CPU and GPU fans to 60%% flat')
            print('  4. Click Apply')
            print('')
            print('  Fans should DROP to the builtin curve (~35-42%%).')
            print('  MechControl\'s apply doesn\'t work on this machine')
            print('  -- that\'s exactly what we\'re investigating.')
            print('')
            print('  Watch the DUTY lines to confirm fans dropped.')
            print('')
            self.marker('step 3 - MechControl broken apply')
            if not self.step_wait():
                self._cleanup(duty_thread)
                return
            self.end_marker()

            # ----------------------------------------------------------
            # STEP 4: Capture GCUService startup
            # ----------------------------------------------------------
            self.step_header(4, TOTAL_STEPS, 'Capture GCUService startup')
            print('')
            print('  Press Enter to start the capture.')
            print('  The script will tell you when to start GCUService.')
            print('')

            # Verify GCUService is not running before capture
            pid = find_pid('GCUService.exe')
            if pid:
                print('[WARN] GCUService is already running (PID=%d)!'
                      % pid)
                print('       Stop it first for a clean capture.')
                print('')

            if not self.step_wait('Press Enter to START CAPTURE: '):
                self._cleanup(duty_thread)
                return

            self.duty_to_console = True
            self.marker('GCUService startup capture')
            self.cout('')
            self.cout('  ================================================')
            self.cout('  >>> Start GCUService NOW <<<')
            self.cout('  ================================================')
            self.cout('')
            self.cout('  In another admin window, run:')
            self.cout('      net start GCUService')
            self.cout('  Or launch the OEM Control Center app.')
            self.cout('')

            success = self.wait_and_attach()

            if not success:
                self.end_marker()
                self.cout('[ERROR] Failed to attach. Aborting.')
                self._cleanup(duty_thread)
                return

            self.cout('')
            self.cout('  Frida attached. Watching ALL EC activity.')
            self.cout('  DUTY lines show fan speed changes in real time.')
            self.cout('')
            self.cout('  Wait for fans to change speed, then wait another')
            self.cout('  30 seconds to capture any follow-up writes.')
            self.cout('')

            read_line('>>> Press Enter when done observing (or "quit"): ')
            self.end_marker()

            print('')
            print('=' * 70)
            print('  ALL 4 STEPS COMPLETE')
            print('=' * 70)

        except KeyboardInterrupt:
            print('\nInterrupted.')

        self._cleanup(duty_thread)

    def _cleanup(self, duty_thread=None):
        self.duty_stop.set()
        if duty_thread:
            duty_thread.join(timeout=2)
        if self.ec_handle:
            kernel32.CloseHandle(self.ec_handle)
            self.ec_handle = None
        if self.frida_session:
            print('\nDetaching Frida...')
            try:
                self.frida_session.detach()
            except Exception:
                pass
        if self.logfile:
            self.logfile.close()
            self.logfile = None
        print('Log saved: ' + LOG_PATH)


if __name__ == '__main__':
    StartupCapture().run()
