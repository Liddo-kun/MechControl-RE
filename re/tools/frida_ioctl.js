var seenCodes = {};

function hex8(v) { return '0x' + ('00000000' + (v >>> 0).toString(16)).slice(-8); }

function hookNt() {
    var m = Process.getModuleByName('ntdll.dll');
    var f = m.findExportByName('NtDeviceIoControlFile');
    if (!f) { send({ t: 'err', e: 'no NtDeviceIoControlFile' }); return; }
    Interceptor.attach(f, {
        onEnter: function (a) {
            this.handle = a[0];
            this.code = a[5].toUInt32() >>> 0;
            this.inBuf = a[6]; this.inLen = a[7].toUInt32();
            this.outBuf = a[8]; this.outLen = a[9].toUInt32();
            this.inHex = '';
            if (!this.inBuf.isNull() && this.inLen > 0 && this.inLen <= 256) {
                try { this.inHex = hexdump(this.inBuf, { length: this.inLen, header: false, ansi: false }); } catch (e) { }
            }
        },
        onLeave: function (rv) {
            var key = hex8(this.code);
            var rec = { t: 'ioctl', code: key, inLen: this.inLen, outLen: this.outLen, status: rv.toInt32() };
            if (this.inHex) rec.inHex = this.inHex;
            if (!this.outBuf.isNull() && this.outLen > 0 && this.outLen <= 256) {
                try { rec.outHex = hexdump(this.outBuf, { length: this.outLen, header: false, ansi: false }); } catch (e) { }
            }
            send(rec);
        }
    });
    send({ t: 'info', msg: 'hooked NtDeviceIoControlFile' });
}
hookNt();
