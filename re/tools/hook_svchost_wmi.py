"""
Hook the svchost.exe process hosting the WMI service (winmgmt).
Captures IOCTL_WMI_EXECUTE_METHOD (0x224014) to find the exact
UInt64 parameter for GetSetULong local dimming.
Also captures nearby IOCTLs in 0x224xxx range.
Each entry flushed immediately to JSONL.
"""
import frida, sys, subprocess, time, json

JS = r"""
'use strict';
var fn = Process.findModuleByName('ntdll.dll').getExportByName('NtDeviceIoControlFile');
Interceptor.attach(fn, {
    onEnter: function(a) {
        this.ioctl = a[5].toUInt32();
        this.ib = a[6]; this.is = a[7].toUInt32();
        this.ob = a[8]; this.os = a[9].toUInt32();
    },
    onLeave: function(ret) {
        var ioctl = this.ioctl;
        // Capture WMI IOCTLs (0x224xxx range)
        if ((ioctl & 0xFFF000) !== 0x224000) return;

        var inBytes = [];
        for (var i = 0; i < Math.min(this.is, 1024); i++) {
            try { inBytes.push(this.ib.add(i).readU8()); } catch(e) { break; }
        }
        var outBytes = [];
        for (var i = 0; i < Math.min(this.os, 1024); i++) {
            try { outBytes.push(this.ob.add(i).readU8()); } catch(e) { break; }
        }

        // Highlight EXECUTE_METHOD (0x224014)
        var isExec = (ioctl === 0x224014);

        send({
            t: 'wmi',
            ioctl: '0x' + ('00000000' + ioctl.toString(16)).slice(-8),
            exec: isExec,
            inSz: this.is, outSz: this.os,
            inB: inBytes, outB: outBytes,
            ret: ret.toUInt32()
        });
    }
});
send({t: 'ready'});
"""

OUTPATH = 'C:/Users/Jon/clean/svchost_wmi_dump.jsonl'

def on_message(msg, data):
    if msg['type'] == 'send':
        p = msg['payload']
        if p.get('t') == 'ready':
            print("[*] Hook ready. Toggle dimming now...")
        elif p.get('t') == 'wmi':
            outfile.write(json.dumps(p) + '\n')
            outfile.flush()
            marker = ' *** EXECUTE_METHOD ***' if p.get('exec') else ''
            print(f"  [{p['ioctl']}] in={p['inSz']} out={p['outSz']} ret={p['ret']}{marker}")
    elif msg['type'] == 'error':
        print(f"[ERR] {msg.get('description', '?')}")

def find_wmi_svchost():
    """Find the svchost.exe PID hosting the winmgmt (WMI) service."""
    r = subprocess.run(
        ['powershell', '-Command',
         '(Get-CimInstance Win32_Service -Filter "Name=\'winmgmt\'").ProcessId'],
        capture_output=True, text=True
    )
    if r.returncode != 0 or not r.stdout.strip():
        print("ERROR: Could not find winmgmt service PID")
        print(r.stderr)
        sys.exit(1)
    return int(r.stdout.strip())

def main():
    pid = find_wmi_svchost()
    print(f"[*] WMI service (winmgmt) svchost PID: {pid}")

    global outfile
    outfile = open(OUTPATH, 'w')

    session = frida.attach(pid)
    script = session.create_script(JS)
    script.on('message', on_message)
    script.load()

    print(f"[*] Filtering for WMI IOCTLs (0x224xxx), especially 0x224014 (EXECUTE_METHOD)")
    print(f"[*] Toggle dimming ON then OFF. Kill whenever.")
    print(f"[*] Output: {OUTPATH}")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass

    try:
        session.detach()
    except:
        pass
    outfile.close()
    print(f"[*] Done. Saved to {OUTPATH}")

if __name__ == '__main__':
    main()
