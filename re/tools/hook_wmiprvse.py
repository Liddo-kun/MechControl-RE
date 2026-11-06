"""
Hook wmiprvse.exe to capture WMI method execution parameters.
Captures the IOCTL_WMI_METHOD_EXECUTE (0x224014) calls to find
the exact UInt64 parameter for local dimming GetSetULong.
"""
import frida
import sys
import subprocess
import time

JS_HOOK = r"""
'use strict';

function findExport(dll, name) {
    var mod = Process.findModuleByName(dll);
    if (!mod) return null;
    try { return mod.getExportByName(name); }
    catch(e) { return null; }
}

function hexBytes(ptr, len) {
    if (ptr.isNull() || len <= 0) return '';
    var s = '', n = Math.min(len, 256);
    for (var i = 0; i < n; i++) {
        try {
            var b = ptr.add(i).readU8();
            s += (b < 16 ? '0' : '') + b.toString(16).toUpperCase() + ' ';
        } catch(e) { s += '?? '; break; }
    }
    return s.trim();
}

// Hook NtDeviceIoControlFile in wmiprvse.exe
var fn = findExport('ntdll.dll', 'NtDeviceIoControlFile');
if (!fn) {
    send({type:'error', msg:'NtDeviceIoControlFile not found'});
} else {
    Interceptor.attach(fn, {
        onEnter: function(args) {
            this.ioctl = args[5].toUInt32();
            this.inBuf = args[6];
            this.inSize = args[7].toUInt32();
            this.outBuf = args[8];
            this.outSize = args[9].toUInt32();
        },
        onLeave: function(ret) {
            var ioctl = this.ioctl;
            // IOCTL_WMI_METHOD_EXECUTE = 0x224014
            // Also check 0x224000-0x224FFF range for other WMI IOCTLs
            if ((ioctl & 0xFFF000) !== 0x224000) return;

            var ioctlHex = '0x' + ioctl.toString(16).toUpperCase();
            var inData = hexBytes(this.inBuf, Math.min(this.inSize, 256));

            // For WNODE_METHOD_ITEM, the structure has:
            // offset 0: WNODE_HEADER (0x30 bytes for x64)
            //   0x00: BufferSize (uint32)
            //   0x04: ProviderId (uint32)
            //   ...
            //   0x10: Guid (16 bytes)
            //   0x20: Flags (uint32)
            //   ...
            // offset 0x30: OffsetInstanceName (uint32)
            // offset 0x34: SizeInstanceName (uint32)
            // offset 0x38: MethodId (uint32)
            // offset 0x3C: SizeDataBlock (uint32)
            // offset 0x40: DataBlockOffset (uint32)

            var methodId = 0;
            var dataU64 = '?';
            var sizeDataBlock = 0;
            var guid = '';

            if (this.inSize >= 0x48 && !this.inBuf.isNull()) {
                try {
                    // Read GUID at offset 0x10
                    guid = hexBytes(this.inBuf.add(0x10), 16);

                    methodId = this.inBuf.add(0x38).readU32();
                    sizeDataBlock = this.inBuf.add(0x3C).readU32();
                    var dataBlockOffset = this.inBuf.add(0x40).readU32();

                    if (sizeDataBlock >= 8 && dataBlockOffset > 0 && dataBlockOffset + 8 <= this.inSize) {
                        var lo = this.inBuf.add(dataBlockOffset).readU32();
                        var hi = this.inBuf.add(dataBlockOffset + 4).readU32();
                        dataU64 = '0x' + (hi * 0x100000000 + lo).toString(16).toUpperCase() +
                                  ' (lo=0x' + lo.toString(16).toUpperCase() +
                                  ' hi=0x' + hi.toString(16).toUpperCase() + ')';
                    } else if (sizeDataBlock >= 4 && dataBlockOffset > 0) {
                        var val = this.inBuf.add(dataBlockOffset).readU32();
                        dataU64 = '0x' + val.toString(16).toUpperCase() + ' (uint32)';
                    }
                } catch(e) {
                    dataU64 = 'read-error: ' + e;
                }
            }

            // Read output after the call
            var outData = '';
            if (this.outSize >= 4 && !this.outBuf.isNull()) {
                try {
                    outData = hexBytes(this.outBuf, Math.min(this.outSize, 128));
                } catch(e) {}
            }

            send({
                type: 'wmi_ioctl',
                ioctl: ioctlHex,
                methodId: methodId,
                data: dataU64,
                sizeDataBlock: sizeDataBlock,
                guid: guid,
                inSize: this.inSize,
                outSize: this.outSize,
                inDump: inData,
                outDump: outData,
                ret: ret.toUInt32()
            });
        }
    });
    send({type:'info', msg:'Hooked NtDeviceIoControlFile in wmiprvse.exe'});
}

send({type:'info', msg:'Ready. Toggle local dimming now...'});
"""

def on_message(message, data):
    if message['type'] == 'send':
        p = message['payload']
        if p.get('type') == 'info':
            print(f"[INFO] {p['msg']}")
        elif p.get('type') == 'wmi_ioctl':
            print(f"\n{'='*70}")
            print(f"[WMI IOCTL] {p['ioctl']}  MethodId={p['methodId']}  SizeDataBlock={p['sizeDataBlock']}")
            print(f"  GUID: {p['guid']}")
            print(f"  Data(UInt64): {p['data']}")
            print(f"  Input ({p['inSize']} bytes): {p['inDump'][:200]}")
            print(f"  Output ({p['outSize']} bytes): {p['outDump'][:200]}")
            print(f"  Return: {p['ret']}")
            print(f"{'='*70}")
        elif p.get('type') == 'error':
            print(f"[ERROR] {p['msg']}")
        else:
            print(f"[MSG] {p}")
    elif message['type'] == 'error':
        print(f"[FRIDA ERROR] {message.get('description','?')}")

def main():
    # Find wmiprvse.exe PID
    result = subprocess.run(
        ['powershell', '-Command', '(Get-Process -Name wmiprvse -ErrorAction SilentlyContinue).Id'],
        capture_output=True, text=True
    )
    pids = [int(x.strip()) for x in result.stdout.strip().split('\n') if x.strip()]
    if not pids:
        print("ERROR: wmiprvse.exe not running")
        sys.exit(1)

    print(f"Found wmiprvse.exe PIDs: {pids}")

    sessions = []
    for pid in pids:
        try:
            session = frida.attach(pid)
            script = session.create_script(JS_HOOK)
            script.on('message', on_message)
            script.load()
            sessions.append(session)
            print(f"[*] Attached to wmiprvse.exe PID {pid}")
        except Exception as e:
            print(f"[!] Failed to attach to PID {pid}: {e}")

    if not sessions:
        print("ERROR: Could not attach to any wmiprvse.exe")
        sys.exit(1)

    print("\n[*] Toggle local dimming now. Press Ctrl+C to stop.\n")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nDetaching...")
        for s in sessions:
            try: s.detach()
            except: pass

if __name__ == '__main__':
    main()
