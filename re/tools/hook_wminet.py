"""
Hook wminet_utils.dll in GCUService.exe - the .NET WMI native bridge.
This DLL bridges System.Management managed calls to native COM WMI.
Also hooks IWbemServices::ExecMethod via COM vtable scanning.
"""
import frida, sys, subprocess, time, json

JS = r"""
'use strict';

// ===== APPROACH 1: Hook all wminet_utils.dll exports =====
var wminet = Process.findModuleByName('wminet_utils.dll');
if (wminet) {
    var exps = wminet.enumerateExports();
    send({t: 'info', msg: 'wminet_utils.dll exports (' + exps.length + '): ' +
        exps.map(function(e){return e.name;}).join(', ')});

    // Hook every exported function
    exps.forEach(function(exp) {
        if (exp.type !== 'function') return;
        try {
            Interceptor.attach(exp.address, {
                onEnter: function(args) {
                    this.name = exp.name;
                    this.a = [];
                    for (var i = 0; i < 10; i++) {
                        try { this.a.push(args[i]); } catch(e) { break; }
                    }
                },
                onLeave: function(ret) {
                    // Try to read string args (BSTRs are common in WMI)
                    var argStrs = [];
                    for (var i = 0; i < this.a.length; i++) {
                        var p = this.a[i];
                        if (p.isNull()) { argStrs.push('null'); continue; }
                        try {
                            // Try reading as wide string (BSTR)
                            var s = p.readUtf16String();
                            if (s && s.length > 0 && s.length < 200) {
                                argStrs.push('"' + s + '"');
                                continue;
                            }
                        } catch(e) {}
                        argStrs.push('0x' + p.toString(16));
                    }
                    send({
                        t: 'wminet',
                        name: this.name,
                        args: argStrs,
                        ret: '0x' + ret.toString(16)
                    });
                }
            });
        } catch(e) {}
    });
} else {
    send({t: 'info', msg: 'wminet_utils.dll NOT loaded!'});
}

// ===== APPROACH 2: Hook IWbemServices via fastprox.dll =====
// In fastprox.dll, look for ExecMethod-related exports
var fastprox = Process.findModuleByName('fastprox.dll');
if (fastprox) {
    var fpExps = fastprox.enumerateExports();
    var execExps = fpExps.filter(function(e) {
        return e.name.indexOf('Exec') >= 0 || e.name.indexOf('Method') >= 0 ||
               e.name.indexOf('Invoke') >= 0;
    });
    send({t: 'info', msg: 'fastprox ExecMethod exports: ' +
        execExps.map(function(e){return e.name;}).join(', ')});

    execExps.forEach(function(exp) {
        if (exp.type !== 'function') return;
        try {
            Interceptor.attach(exp.address, {
                onEnter: function(args) {
                    this.name = 'fastprox!' + exp.name;
                    this.a = [];
                    for (var i = 0; i < 8; i++) {
                        try { this.a.push(args[i]); } catch(e) { break; }
                    }
                },
                onLeave: function(ret) {
                    var argStrs = this.a.map(function(p) {
                        if (p.isNull()) return 'null';
                        try {
                            var s = p.readUtf16String();
                            if (s && s.length > 0 && s.length < 200) return '"' + s + '"';
                        } catch(e) {}
                        return '0x' + p.toString(16);
                    });
                    send({
                        t: 'fastprox',
                        name: this.name,
                        args: argStrs,
                        ret: '0x' + ret.toString(16)
                    });
                }
            });
        } catch(e) {}
    });
}

// ===== APPROACH 3: Hook wbemsvc.dll exports =====
var wbemsvc = Process.findModuleByName('wbemsvc.dll');
if (wbemsvc) {
    var wsExps = wbemsvc.enumerateExports();
    var wsMethodExps = wsExps.filter(function(e) {
        return e.name.indexOf('Exec') >= 0 || e.name.indexOf('Method') >= 0;
    });
    send({t: 'info', msg: 'wbemsvc ExecMethod exports: ' +
        wsMethodExps.map(function(e){return e.name;}).join(', ')});

    wsMethodExps.forEach(function(exp) {
        if (exp.type !== 'function') return;
        try {
            Interceptor.attach(exp.address, {
                onEnter: function(args) {
                    this.name = 'wbemsvc!' + exp.name;
                    this.a = [];
                    for (var i = 0; i < 8; i++) {
                        try { this.a.push(args[i]); } catch(e) { break; }
                    }
                },
                onLeave: function(ret) {
                    var argStrs = this.a.map(function(p) {
                        if (p.isNull()) return 'null';
                        try {
                            var s = p.readUtf16String();
                            if (s && s.length > 0 && s.length < 200) return '"' + s + '"';
                        } catch(e) {}
                        return '0x' + p.toString(16);
                    });
                    send({
                        t: 'wbemsvc',
                        name: this.name,
                        args: argStrs,
                        ret: '0x' + ret.toString(16)
                    });
                }
            });
        } catch(e) {}
    });
}

// ===== APPROACH 4: Hook NdrClientCall3 in rpcrt4.dll =====
// This is the RPC marshaling call used by ALL COM proxy methods.
// We filter by checking if the method name contains 'GetSetULong' or similar.
var rpcrt4 = Process.findModuleByName('rpcrt4.dll');
if (rpcrt4) {
    ['NdrClientCall3', 'NdrClientCall2', 'Ndr64AsyncClientCall', 'NdrClientCall4'].forEach(function(fname) {
        try {
            var fn = rpcrt4.getExportByName(fname);
            Interceptor.attach(fn, {
                onEnter: function(args) {
                    this.fname = fname;
                    // For NdrClientCall3: args[0]=pProxyInfo, args[1]=nProcNum
                    // For NdrClientCall2: args[0]=pStubDescriptor, args[1]=pFormat
                    this.procNum = args[1].toUInt32();
                    // We only care about specific proc numbers
                    // ExecMethod = slot 24, ExecMethodAsync = slot 25
                    // In COM proxy, procNum typically = vtable slot * 2 + 3 or similar
                    this.triggered = false;
                },
                onLeave: function(ret) {
                    if (this.triggered) {
                        send({
                            t: 'rpc',
                            name: this.fname,
                            procNum: this.procNum,
                            ret: '0x' + ret.toString(16)
                        });
                    }
                }
            });
            send({t: 'info', msg: 'Hooked ' + fname});
        } catch(e) {}
    });
}

send({t: 'ready'});
"""

OUTPATH = 'C:/Users/Jon/clean/wminet_dump.jsonl'

def on_message(msg, data):
    if msg['type'] == 'send':
        p = msg['payload']
        if p.get('t') == 'ready':
            print("[*] Hook ready. Toggle dimming now...")
        elif p.get('t') == 'info':
            print(f"[INFO] {p['msg']}")
        elif p.get('t') in ('wminet', 'fastprox', 'wbemsvc', 'rpc'):
            outfile.write(json.dumps(p) + '\n')
            outfile.flush()
            args = ', '.join(str(a)[:50] for a in p.get('args', [])[:5])
            print(f"  [{p['t']}] {p['name']}({args}) -> {p.get('ret','?')}")
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
