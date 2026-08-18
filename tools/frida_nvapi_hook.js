var hooked = {};

function hex8(v) { return '0x' + ('00000000' + (v >>> 0).toString(16)).slice(-8); }

function tryHook() {
    var qi = null;
    var m = Process.findModuleByName('nvapi64.dll');
    if (m) qi = m.findExportByName('nvapi_QueryInterface');
    if (!qi) { setTimeout(tryHook, 500); return; }
    Interceptor.attach(qi, {
        onEnter: function (args) { this.id = args[0].toUInt32() >>> 0; },
        onLeave: function (ret) {
            send({ t: 'qi', id: hex8(this.id), ret: ret.isNull() ? 'NULL' : 'ok' });
            if (ret.isNull()) return;
            var key = ret.toString();
            if (hooked[key]) return;
            hooked[key] = this.id;
            var fid = this.id;
            Interceptor.attach(ret, {
                onEnter: function (a) {
                    this.buf = null; this.sz = 0;
                    try {
                        var v = a[1].readU32();
                        var sz = v & 0xffff, ver = v >>> 16;
                        if (sz >= 4 && sz <= 8192 && ver >= 1 && ver <= 10) { this.buf = a[1]; this.sz = sz; }
                    } catch (e) { }
                },
                onLeave: function (rv) {
                    var m = { t: 'call', id: hex8(fid), rv: rv.toInt32() };
                    if (this.buf) {
                        try { m.hex = hexdump(this.buf, { length: Math.min(this.sz, 512), header: false, ansi: false }); } catch (e) { }
                    }
                    send(m);
                }
            });
        }
    });
    send({ t: 'info', msg: 'hooked nvapi_QueryInterface' });
}
tryHook();
