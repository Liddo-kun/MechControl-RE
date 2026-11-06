send({ t: "hello" });
try {
    var p = Module.getExportByName("kernel32.dll", "DeviceIoControl");
    send({ t: "mod", n: p.toString() });
} catch (e) {
    send({ t: "err", e: e.toString() });
}
