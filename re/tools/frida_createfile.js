function hookCreate(moduleName) {
    var m;
    try { m = Process.getModuleByName(moduleName); } catch (e) { return; }
    ['CreateFileW', 'CreateFileA'].forEach(function (name) {
        var f = m.findExportByName(name);
        if (!f) return;
        Interceptor.attach(f, {
            onEnter: function (a) {
                try {
                    var p = name === 'CreateFileW' ? a[0].readUtf16String() : a[0].readCString();
                    if (p && p.indexOf('\\') === 0 || (p && p.toLowerCase().indexOf('cpuz') >= 0) || (p && p.toLowerCase().indexOf('cpuid') >= 0))
                        send({ t: 'create', api: name, path: p });
                } catch (e) { }
            }
        });
    });
}
hookCreate('kernel32.dll');
hookCreate('KERNELBASE.dll');
send({ t: 'info', msg: 'createfile hooks in' });
