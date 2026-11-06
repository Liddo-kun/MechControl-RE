"""
Hook WMI syscalls in GCUService.exe to capture the actual method execution.
Tries: NtWmi* exports, IoWMI* exports, and COM vtable approach.
"""
import frida, sys, subprocess, time, json

JS = r"""
'use strict';

var ntdll = Process.findModuleByName('ntdll.dll');
var hooked = [];

// Step 1: Find ALL ntdll exports containing 'Wmi' or 'WMI'
var exports = ntdll.enumerateExports();
var wmiExports = exports.filter(function(e) {
    return e.name.indexOf('Wmi') >= 0 || e.name.indexOf('WMI') >= 0 || e.name.indexOf('wmi') >= 0;
});
send({t: 'info', msg: 'ntdll WMI exports: ' + wmiExports.map(function(e){return e.name;}).join(', ')});

// Hook every NtWmi* export
wmiExports.forEach(function(exp) {
    if (exp.type !== 'function') return;
    try {
        Interceptor.attach(exp.address, {
            onEnter: function(args) {
                this.name = exp.name;
                this.args = [];
                // Capture first 8 args as raw pointers
                for (var i = 0; i < 8; i++) {
                    this.args.push(args[i]);
                }
            },
            onLeave: function(ret) {
                var argVals = this.args.map(function(a) {
                    return '0x' + a.toString(16);
                });
                send({
                    t: 'wmi_syscall',
                    name: this.name,
                    args: argVals,
                    ret: '0x' + (ret.toUInt32()).toString(16)
                });
            }
        });
        hooked.push(exp.name);
    } catch(e) {
        send({t: 'info', msg: 'Failed to hook ' + exp.name + ': ' + e});
    }
});

// Step 2: Also check for WMI-related functions in advapi32, kernel32
['advapi32.dll', 'kernel32.dll', 'wmiutils.dll', 'wbemsvc.dll', 'wbemcomn.dll', 'wbemprox.dll'].forEach(function(dll) {
    var mod = Process.findModuleByName(dll);
    if (!mod) return;
    var exps = mod.enumerateExports();
    var wmiExps = exps.filter(function(e) {
        return e.name.indexOf('Wmi') >= 0 || e.name.indexOf('WMI') >= 0 ||
               e.name.indexOf('ExecMethod') >= 0 || e.name.indexOf('ExecuteMethod') >= 0;
    });
    if (wmiExps.length > 0) {
        send({t: 'info', msg: dll + ' WMI exports: ' + wmiExps.map(function(e){return e.name;}).join(', ')});
        wmiExps.forEach(function(exp) {
            if (exp.type !== 'function') return;
            try {
                Interceptor.attach(exp.address, {
                    onEnter: function(args) {
                        this.name = dll + '!' + exp.name;
                        this.args = [];
                        for (var i = 0; i < 8; i++) {
                            this.args.push(args[i]);
                        }
                    },
                    onLeave: function(ret) {
                        var argVals = this.args.map(function(a) {
                            return '0x' + a.toString(16);
                        });
                        send({
                            t: 'wmi_api',
                            name: this.name,
                            args: argVals,
                            ret: '0x' + (ret.toUInt32()).toString(16)
                        });
                    }
                });
                hooked.push(dll + '!' + exp.name);
            } catch(e) {}
        });
    }
});

// Step 3: List ALL loaded modules for reference
var allMods = Process.enumerateModules();
var interestingMods = allMods.filter(function(m) {
    var n = m.name.toLowerCase();
    return n.indexOf('wmi') >= 0 || n.indexOf('wbem') >= 0 || n.indexOf('fastprox') >= 0 ||
           n.indexOf('system.management') >= 0 || n.indexOf('acpi') >= 0;
});
send({t: 'info', msg: 'WMI-related modules: ' + interestingMods.map(function(m){return m.name;}).join(', ')});

send({t: 'info', msg: 'Hooked: ' + hooked.join(', ')});
send({t: 'ready'});
"""

OUTPATH = 'C:/Users/Jon/clean/wmi_syscall_dump.jsonl'

def on_message(msg, data):
    if msg['type'] == 'send':
        p = msg['payload']
        if p.get('t') == 'ready':
            print("[*] Hook ready. Toggle dimming now...")
        elif p.get('t') == 'info':
            print(f"[INFO] {p['msg']}")
        elif p.get('t') in ('wmi_syscall', 'wmi_api'):
            outfile.write(json.dumps(p) + '\n')
            outfile.flush()
            args_str = ', '.join(p['args'][:4])
            print(f"  [{p['name']}] args=({args_str}) ret={p['ret']}")
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

    print(f"[*] Toggle dimming. Kill whenever.")
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
    print(f"[*] Done.")

if __name__ == '__main__':
    main()
