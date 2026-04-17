"""
Frida script to hook WMI InvokeMethod calls in GCUService.exe
to capture GetSetULong parameter values for local dimming.
"""
import frida
import sys
import time

JS_HOOK = r"""
'use strict';

// Hook IWbemServices::ExecMethod (COM vtable index 24)
// IWbemServices vtable:
//   0: QueryInterface, 1: AddRef, 2: Release
//   3-23: other IWbemServices methods
//   24: ExecMethod
//   25: ExecMethodAsync

// We need to find the IWbemServices pointer first by hooking
// IWbemLocator::ConnectServer which returns an IWbemServices pointer

var hooked = false;

// Hook CoCreateInstance to catch WbemLocator creation
var ole32 = Module.findBaseAddress('ole32.dll');
if (!ole32) {
    ole32 = Module.findBaseAddress('combase.dll');
}

// Instead of COM hooking, let's hook fastprox!CWbemSvcWrapper::XWbemServices::ExecMethod
// or wbemprox. But the simplest is to hook the managed ManagementObject.InvokeMethod

// Let's hook at the wbemprox level - IWbemServices::ExecMethod
// Find wbem service functions

var modules = Process.enumerateModules();
var wbemNames = modules.map(m => m.name).filter(n => n.toLowerCase().includes('wbem') || n.toLowerCase().includes('fastprox'));
send({type: 'info', data: 'Loaded wbem modules: ' + JSON.stringify(wbemNames)});

// Hook at the COM level by intercepting ExecMethod and ExecMethodAsync
// The key insight: we can hook IWbemServices_ExecMethod_Proxy in ole32/rpcrt4
// But easier: hook the managed side - System.Management.ManagementObject.InvokeMethod

// For .NET Framework, the managed interop eventually calls into wmicli/fastprox
// Let's try hooking fastprox.dll CWbemObject::Put and ExecMethod

var fastprox = Module.findBaseAddress('fastprox.dll');
if (fastprox) {
    send({type: 'info', data: 'fastprox.dll found at ' + fastprox});

    // Enumerate exports
    var exports = Module.enumerateExports('fastprox.dll');
    var methodExports = exports.filter(e => e.name.includes('Exec') || e.name.includes('Method') || e.name.includes('Put'));
    send({type: 'info', data: 'fastprox method exports: ' + JSON.stringify(methodExports.map(e => e.name).slice(0, 20))});
}

// Better approach: hook wbemprox.dll or the WMI provider wmiprov.dll
var wmiprov = Module.findBaseAddress('wmiprov.dll');
if (wmiprov) {
    send({type: 'info', data: 'wmiprov.dll found at ' + wmiprov});
}

// Most direct approach: Hook DeviceIoControl calls to wmiacpi
// When GetSetULong is called via WMI, it eventually calls the ACPI driver
// through WMI data block method execution, which goes through DeviceIoControl
var kernel32 = Module.findBaseAddress('kernel32.dll');
var DeviceIoControl = Module.findExportByName('kernel32.dll', 'DeviceIoControl');
if (DeviceIoControl) {
    Interceptor.attach(DeviceIoControl, {
        onEnter: function(args) {
            this.handle = args[0];
            this.ioctl = args[1].toInt32() >>> 0;
            this.inBuf = args[2];
            this.inSize = args[3].toInt32();
            this.outBuf = args[4];
            this.outSize = args[5].toInt32();

            // Log all IOCTLs to find the WMI one
            // WMI method calls go through WMIGUID_EXECUTE_METHOD = 0x224014
            // or through wmiacpi specific IOCTLs
            var ioctl_hex = '0x' + this.ioctl.toString(16).toUpperCase();

            // Filter for interesting IOCTLs
            // 0x224014 = IOCTL_WMI_EXECUTE_METHOD (FILE_DEVICE_WMI)
            // 0x9C40xxxx = GPD ACPI driver IOCTLs
            if (this.ioctl === 0x224014 || (this.ioctl >= 0x9C400000 && this.ioctl <= 0x9C40FFFF) ||
                this.ioctl === 0x22400C || this.ioctl === 0x224010) {

                var inData = '';
                if (this.inSize > 0 && this.inSize < 256) {
                    try {
                        inData = hexdump(this.inBuf, {length: Math.min(this.inSize, 128)});
                    } catch(e) {
                        inData = 'read error';
                    }
                }

                send({
                    type: 'ioctl',
                    ioctl: ioctl_hex,
                    inSize: this.inSize,
                    outSize: this.outSize,
                    inData: inData
                });
            }
        },
        onLeave: function(retval) {
        }
    });
    send({type: 'info', data: 'Hooked DeviceIoControl'});
}

send({type: 'info', data: 'Hooks installed. Toggle local dimming now...'});
"""

def on_message(message, data):
    if message['type'] == 'send':
        payload = message['payload']
        if payload.get('type') == 'info':
            print(f"[INFO] {payload['data']}")
        elif payload.get('type') == 'ioctl':
            print(f"\n[IOCTL] Code={payload['ioctl']} InSize={payload['inSize']} OutSize={payload['outSize']}")
            if payload.get('inData'):
                print(f"  Input:\n{payload['inData']}")
        else:
            print(f"[MSG] {payload}")
    elif message['type'] == 'error':
        print(f"[ERROR] {message['description']}")

def main():
    try:
        # Attach to GCUService.exe
        session = frida.attach("GCUService.exe")
        print("[*] Attached to GCUService.exe")

        script = session.create_script(JS_HOOK)
        script.on('message', on_message)
        script.load()
        print("[*] Script loaded. Toggle local dimming now...")
        print("[*] Press Ctrl+C to stop")

        # Wait for events
        while True:
            time.sleep(1)

    except frida.ProcessNotFoundError:
        print("ERROR: GCUService.exe not found. Is the OEM service running?")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n[*] Detaching...")
        session.detach()

if __name__ == '__main__':
    main()
