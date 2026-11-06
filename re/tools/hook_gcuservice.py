"""
Hook GCUService.exe to capture ALL IOCTLs and DDC/CI calls.
Captures NtDeviceIoControlFile (all IOCTL codes) + SetVCPFeature (DDC/CI).
Each entry flushed immediately to JSONL file.
"""
import frida, sys, subprocess, time, json

JS = r"""
'use strict';

function hexU32(v) { return '0x' + ('00000000' + v.toString(16)).slice(-8); }

// === Hook NtDeviceIoControlFile (ALL IOCTLs, not just WMI) ===
var ntdio = Process.findModuleByName('ntdll.dll');
if (ntdio) {
    var fn = ntdio.getExportByName('NtDeviceIoControlFile');
    Interceptor.attach(fn, {
        onEnter: function(a) {
            this.handle = a[0];
            this.ioctl = a[5].toUInt32();
            this.ib = a[6]; this.is = a[7].toUInt32();
            this.ob = a[8]; this.os = a[9].toUInt32();
        },
        onLeave: function(ret) {
            var inBytes = [];
            for (var i = 0; i < Math.min(this.is, 1024); i++) {
                try { inBytes.push(this.ib.add(i).readU8()); } catch(e) { break; }
            }
            var outBytes = [];
            for (var i = 0; i < Math.min(this.os, 1024); i++) {
                try { outBytes.push(this.ob.add(i).readU8()); } catch(e) { break; }
            }
            send({
                t: 'ioctl',
                ioctl: hexU32(this.ioctl),
                inSz: this.is, outSz: this.os,
                inB: inBytes, outB: outBytes,
                ret: ret.toUInt32()
            });
        }
    });
    send({t: 'info', msg: 'Hooked NtDeviceIoControlFile (ALL IOCTLs)'});
}

// === Hook DeviceIoControl (kernel32) as backup ===
var k32dio = Process.findModuleByName('kernel32.dll');
if (k32dio) {
    try {
        var dio = k32dio.getExportByName('DeviceIoControl');
        Interceptor.attach(dio, {
            onEnter: function(a) {
                this.ioctl = a[1].toUInt32();
                this.ib = a[2]; this.is = a[3].toUInt32();
                this.ob = a[4]; this.os = a[5].toUInt32();
            },
            onLeave: function(ret) {
                // Only log if this looks like a non-WMI IOCTL (WMI ones caught by ntdll hook)
                var ioctl = this.ioctl;
                if ((ioctl & 0xFFF000) === 0x224000) return; // skip WMI, already caught
                var inBytes = [];
                for (var i = 0; i < Math.min(this.is, 512); i++) {
                    try { inBytes.push(this.ib.add(i).readU8()); } catch(e) { break; }
                }
                var outBytes = [];
                for (var i = 0; i < Math.min(this.os, 512); i++) {
                    try { outBytes.push(this.ob.add(i).readU8()); } catch(e) { break; }
                }
                send({
                    t: 'ioctl_k32',
                    ioctl: hexU32(ioctl),
                    inSz: this.is, outSz: this.os,
                    inB: inBytes, outB: outBytes,
                    ret: ret.toInt32()
                });
            }
        });
        send({t: 'info', msg: 'Hooked DeviceIoControl (kernel32)'});
    } catch(e) {}
}

// === Hook SetVCPFeature / GetVCPFeature for DDC/CI ===
var dxva2 = Process.findModuleByName('dxva2.dll');
if (dxva2) {
    try {
        var setVCP = dxva2.getExportByName('SetVCPFeature');
        Interceptor.attach(setVCP, {
            onEnter: function(a) {
                this.hMonitor = a[0];
                this.code = a[1].toUInt32();
                this.value = a[2].toUInt32();
            },
            onLeave: function(ret) {
                send({
                    t: 'ddcci_set',
                    code: hexU32(this.code),
                    value: hexU32(this.value),
                    ret: ret.toInt32()
                });
            }
        });
        send({t: 'info', msg: 'Hooked SetVCPFeature (DDC/CI)'});
    } catch(e) { send({t: 'info', msg: 'SetVCPFeature not found: ' + e}); }

    try {
        var getVCP = dxva2.getExportByName('GetVCPFeatureAndVCPFeatureReply');
        Interceptor.attach(getVCP, {
            onEnter: function(a) {
                this.code = a[1].toUInt32();
                this.pType = a[2];
                this.pCur = a[3];
                this.pMax = a[4];
            },
            onLeave: function(ret) {
                var cur = 0, max = 0;
                try { if (!this.pCur.isNull()) cur = this.pCur.readU32(); } catch(e) {}
                try { if (!this.pMax.isNull()) max = this.pMax.readU32(); } catch(e) {}
                send({
                    t: 'ddcci_get',
                    code: hexU32(this.code),
                    cur: cur, max: max,
                    ret: ret.toInt32()
                });
            }
        });
        send({t: 'info', msg: 'Hooked GetVCPFeatureAndVCPFeatureReply (DDC/CI)'});
    } catch(e) {}
} else {
    send({t: 'info', msg: 'dxva2.dll not loaded (no DDC/CI hooks)'});
}

// === Hook WriteFile for any direct driver writes ===
var writeFile = Process.findModuleByName('kernel32.dll');
if (writeFile) {
    try {
        var wf = writeFile.getExportByName('WriteFile');
        Interceptor.attach(wf, {
            onEnter: function(a) {
                this.handle = a[0];
                this.buf = a[1];
                this.size = a[2].toUInt32();
            },
            onLeave: function(ret) {
                // Only log small writes (likely control commands, not file I/O)
                if (this.size > 0 && this.size <= 64) {
                    var bytes = [];
                    for (var i = 0; i < this.size; i++) {
                        try { bytes.push(this.buf.add(i).readU8()); } catch(e) { break; }
                    }
                    send({
                        t: 'write',
                        sz: this.size,
                        data: bytes,
                        ret: ret.toInt32()
                    });
                }
            }
        });
        send({t: 'info', msg: 'Hooked WriteFile (small writes only)'});
    } catch(e) {}
}

// === List loaded modules ===
var mods = Process.enumerateModules();
var interesting = mods.filter(function(m) {
    var n = m.name.toLowerCase();
    return n.indexOf('wmi') >= 0 || n.indexOf('wbem') >= 0 || n.indexOf('acpi') >= 0 ||
           n.indexOf('dxva') >= 0 || n.indexOf('ddc') >= 0 || n.indexOf('monitor') >= 0 ||
           n.indexOf('display') >= 0 || n.indexOf('igfx') >= 0 || n.indexOf('nvapi') >= 0 ||
           n.indexOf('gcu') >= 0 || n.indexOf('gpu') >= 0;
});
send({t: 'info', msg: 'Interesting modules: ' + interesting.map(function(m){return m.name;}).join(', ')});

send({t: 'ready'});
"""

OUTPATH = 'C:/Users/Jon/clean/gcuservice_ioctl_dump.jsonl'

def on_message(msg, data):
    if msg['type'] == 'send':
        p = msg['payload']
        if p.get('t') == 'ready':
            print("[*] ALL hooks ready. Toggle dimming now...")
        elif p.get('t') == 'info':
            print(f"[INFO] {p['msg']}")
        elif p.get('t') == 'ioctl':
            outfile.write(json.dumps(p) + '\n')
            outfile.flush()
            print(f"  [IOCTL {p['ioctl']}] in={p['inSz']} out={p['outSz']} ret={p['ret']}")
        elif p.get('t') == 'ioctl_k32':
            outfile.write(json.dumps(p) + '\n')
            outfile.flush()
            print(f"  [K32  {p['ioctl']}] in={p['inSz']} out={p['outSz']}")
        elif p.get('t') in ('ddcci_set', 'ddcci_get'):
            outfile.write(json.dumps(p) + '\n')
            outfile.flush()
            print(f"  [DDC/CI] {p['t']} code={p['code']} {'value='+p.get('value','') if 'value' in p else 'cur='+str(p.get('cur',''))}")
        elif p.get('t') == 'write':
            outfile.write(json.dumps(p) + '\n')
            outfile.flush()
            hexdata = ' '.join(f'{b:02X}' for b in p['data'][:32])
            print(f"  [WRITE] {p['sz']}B: {hexdata}")
        else:
            print(f"  [?] {p}")
    elif msg['type'] == 'error':
        print(f"[ERR] {msg.get('description', '?')}")

def main():
    r = subprocess.run(
        ['powershell', '-Command', '(Get-Process -Name GCUService).Id'],
        capture_output=True, text=True
    )
    if r.returncode != 0 or not r.stdout.strip():
        print("ERROR: GCUService.exe not running")
        sys.exit(1)
    pid = int(r.stdout.strip().split('\n')[0])
    print(f"[*] GCUService PID: {pid}")

    global outfile
    outfile = open(OUTPATH, 'w')

    session = frida.attach(pid)
    script = session.create_script(JS)
    script.on('message', on_message)
    script.load()

    print(f"[*] Toggle dimming ON then OFF. Kill this any way you like - data is flushed.")
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
