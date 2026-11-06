import frida, sys, time, subprocess

SCRIPT_JS = """
var mod = Process.getModuleByName('ntdll.dll');
var addr = mod.getExportByName('NtDeviceIoControlFile');

// NtDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock,
//                       IoControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength)
Interceptor.attach(addr, {
    onEnter: function(args) {
        this.ioctl = args[5].toUInt32();
        this.inBuf = args[6];
        this.inSize = args[7].toUInt32();
        this.outBuf = args[8];
        this.outSize = args[9].toUInt32();
        this.isAcpi = ((this.ioctl >>> 16) === 0x9C40);
    },
    onLeave: function(retval) {
        if (!this.isAcpi) return;

        var ioctl = this.ioctl;
        var name = "IOCTL_0x" + ioctl.toString(16).toUpperCase();
        if (ioctl === 0x9C40A488) name = "ECREAD";
        else if (ioctl === 0x9C40A48C) name = "ECWRITE";
        else if (ioctl === 0x9C40A504) name = "CUSTOMCTL";

        var iHex = "";
        if (this.inSize > 0 && !this.inBuf.isNull()) {
            for (var i = 0; i < this.inSize && i < 16; i++) {
                var b = this.inBuf.add(i).readU8();
                var h = b.toString(16).toUpperCase();
                iHex += (h.length < 2 ? "0" + h : h) + " ";
            }
        }

        var oHex = "";
        if (this.outSize > 0 && !this.outBuf.isNull()) {
            for (var i = 0; i < this.outSize && i < 16; i++) {
                try {
                    var b = this.outBuf.add(i).readU8();
                    var h = b.toString(16).toUpperCase();
                    oHex += (h.length < 2 ? "0" + h : h) + " ";
                } catch(e) { break; }
            }
        }

        // Skip repetitive polling reads (0x7CC polled constantly)
        if (name === "ECREAD") {
            var addrVal = 0;
            if (this.inSize >= 4 && !this.inBuf.isNull()) {
                addrVal = this.inBuf.readU32();
            }
            if (addrVal === 0x7CC) return;
        }

        send(name + " | in(" + this.inSize + ")=[" + iHex + "] out(" + this.outSize + ")=[" + oHex + "] ret=" + retval.toUInt32());
    }
});

send('hooked');
"""

def on_message(msg, data):
    if msg['type'] == 'send':
        line = msg['payload']
        if 'ECWRITE' in line:
            out = "\n" + "=" * 60 + "\n*** " + line + " ***\n" + "=" * 60 + "\n"
            print(out)
            logfile.write(out + "\n")
        else:
            print(line)
            logfile.write(line + "\n")
        logfile.flush()
    else:
        print("[ERR] " + str(msg))
    sys.stdout.flush()

result = subprocess.run(['powershell', '-Command', '(Get-Process -Name GCUService).Id'],
    capture_output=True, text=True)
pid = int(result.stdout.strip().split('\n')[0])
print(f"PID {pid}")
session = frida.attach(pid)
script = session.create_script(SCRIPT_JS)
script.on('message', on_message)
script.load()
logfile = open('C:/Users/Jon/clean/ec_trace.log', 'w')
print("Monitoring ACPI calls. Output also saved to ec_trace.log\n")
try:
    while True:
        time.sleep(0.5)
except KeyboardInterrupt:
    logfile.close()
    session.detach()
