var TARGET_IOCTL = 0x9c4025d4;
var THERMAL_OFFSETS = {
    0x00ad0a90: true,
    0x00ad0a94: true,
    0x00ad0a98: true,
    0x00ad0a9c: true,
    0x00ad0aa0: true,
    0x00ad0aa4: true,
    0x00ad0ad4: true,
    0x00ad0ad8: true,
    0x00ad0adc: true,
    0x00ad0ae0: true
};

var ntdll = Process.getModuleByName('ntdll.dll');
var ntDeviceIoControlFile = ntdll.findExportByName('NtDeviceIoControlFile');

Interceptor.attach(ntDeviceIoControlFile, {
    onEnter: function (args) {
        this.capture = false;
        if ((args[5].toUInt32() >>> 0) !== TARGET_IOCTL) return;

        var input = args[6];
        if (input.isNull() || args[7].toUInt32() < 16) return;
        var bdf = input.readU32();
        var bar = input.add(4).readU32();
        var offset = input.add(12).readU32();
        if (bdf !== 0x80020000 || bar !== 0x10 || !THERMAL_OFFSETS[offset]) return;

        this.capture = true;
        this.offset = offset;
        this.output = args[8];
        this.outputLength = args[9].toUInt32();
    },
    onLeave: function (status) {
        if (!this.capture || this.output.isNull() || this.outputLength < 8) return;
        var ok = this.output.readU32();
        var raw = this.output.add(4).readU32();
        var allOnes = raw === 0xffffffff;
        var valid = !allOnes && (raw & 0x40000000) !== 0;
        send({
            t: 'blackwell-temp',
            timeMs: Date.now(),
            offset: '0x' + this.offset.toString(16),
            status: status.toInt32(),
            ok: ok,
            raw: '0x' + raw.toString(16),
            allOnes: allOnes,
            valid: valid,
            tempC: valid ? (raw & 0xffff) / 256.0 : null
        });
    }
});

send({ t: 'info', msg: 'Blackwell thermal capture active' });
