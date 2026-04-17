'use strict';

// ============================================================================
// frida_full_hook.js - Comprehensive GCUService.exe instrumentation
// Hooks: EC (IOCTL), USB HID, DDC/CI, WMI detection, device file opens
// ============================================================================

// ---------- Configuration ----------
var logAllIoctl = false;

// Known EC polling addresses - read every ~250ms by GCUService
// Category 'ec_poll' so Python wrapper can hide them from console
var EC_POLL_ADDRS = {
    0x7CC: true, 0x727: true, 0x7CA: true, 0x74C: true,
    0x766: true, 0x765: true, 0x490: true, 0x4AB: true,
    0x75B: true, 0x75C: true, 0x464: true, 0x465: true,
    0x46B: true, 0x46C: true, 0x463: true, 0x768: true,
    0x742: true, 0x78E: true, 0x7C5: true, 0x7A6: true
};

// ---------- Safe export resolver ----------
// Module.findExportByName is unavailable in this Frida build.
// Use Process.findModuleByName + mod.getExportByName instead.
function findExport(dllName, funcName) {
    var mod = Process.findModuleByName(dllName);
    if (!mod) return null;
    try { return mod.getExportByName(funcName); }
    catch (e) { return null; }
}

// ---------- Utilities ----------
var handleMap = {};

function hex(ptr, len) {
    if (ptr.isNull() || len <= 0) return '';
    var s = '', n = Math.min(len, 512);
    for (var i = 0; i < n; i++) {
        try {
            var b = ptr.add(i).readU8();
            s += (b < 16 ? '0' : '') + b.toString(16).toUpperCase() + ' ';
        } catch (e) { s += '?? '; break; }
    }
    if (len > 512) s += '...(' + len + ' total)';
    return s.trim();
}

function ts() {
    var d = new Date();
    return ('0' + d.getHours()).slice(-2) + ':' +
           ('0' + d.getMinutes()).slice(-2) + ':' +
           ('0' + d.getSeconds()).slice(-2) + '.' +
           ('00' + d.getMilliseconds()).slice(-3);
}

function hk(h) { return h.toString(); }

function emit(cat, text) {
    send({ cat: cat, text: '[' + ts() + '] ' + text });
}

function info(text) {
    send({ cat: 'setup', text: text });
}

// Decode EC address to human-readable name
function ecName(addr) {
    var names = {
        0x706: 'BIOS_CTRL', 0x726: 'OEM9', 0x727: 'ESHUTTER',
        0x730: 'PL1_DEF_GAM', 0x731: 'PL2_DEF_GAM', 0x732: 'PL4_DEF_GAM',
        0x738: 'CPU_TAU', 0x740: 'PROJECT_ID', 0x743: 'DB_CTRL', 0x744: 'TGP',
        0x745: 'DB_TOTAL', 0x746: 'DB_MAX', 0x751: 'FAN_CTRL', 0x767: 'TRIGGER',
        0x768: 'STATUS', 0x769: 'LOGO_R', 0x76A: 'LOGO_G', 0x76B: 'LOGO_B',
        0x783: 'PL1', 0x784: 'PL2', 0x785: 'PL4',
        0x786: 'TCC_OFFSET', 0x787: 'FAN_SWITCH_SPEED',
        0x78C: 'OEM2_TURBO', 0x7A6: 'BATT_MODE', 0x7AB: 'LOGO_ONOFF',
        0x7B9: 'CHRG_LIMIT_UP', 0x7C5: 'OEM5_FANCURVE', 0x7C6: 'OEM6_APPLY',
        0x7D0: 'CHRG_LIMIT_DN', 0x7D8: 'TCC_DEF_GAM', 0x7D9: 'TCC_DEF_OFF'
    };
    if (addr >= 0xF00 && addr <= 0xF0F) return 'CPU_TEMP_UP[' + (addr - 0xF00) + ']';
    if (addr >= 0xF10 && addr <= 0xF1F) return 'CPU_TEMP_DN[' + (addr - 0xF10) + ']';
    if (addr >= 0xF20 && addr <= 0xF2F) return 'CPU_DUTY[' + (addr - 0xF20) + ']';
    if (addr >= 0xF30 && addr <= 0xF3F) return 'GPU_TEMP_UP[' + (addr - 0xF30) + ']';
    if (addr >= 0xF40 && addr <= 0xF4F) return 'GPU_TEMP_DN[' + (addr - 0xF40) + ']';
    if (addr >= 0xF50 && addr <= 0xF5F) return 'GPU_DUTY[' + (addr - 0xF50) + ']';
    return names[addr] || null;
}

// ============================================================================
// 1. CreateFileW - track handle-to-device-path mapping
// ============================================================================
(function () {
    var fn = findExport('kernel32.dll', 'CreateFileW');
    if (!fn) { info('[WARN] CreateFileW not found'); return; }
    Interceptor.attach(fn, {
        onEnter: function (a) {
            try { this.path = a[0].readUtf16String(); } catch (e) { this.path = null; }
        },
        onLeave: function (ret) {
            // INVALID_HANDLE_VALUE on x64 = 0xFFFFFFFFFFFFFFFF
            if (ret.isNull() || ret.equals(ptr('0xFFFFFFFFFFFFFFFF'))) return;
            var p = this.path;
            if (!p) return;
            var pl = p.toLowerCase();
            if (pl.indexOf('hid') !== -1 || pl.indexOf('acpi') !== -1 ||
                pl.indexOf('monitor') !== -1 || pl.indexOf('display') !== -1 ||
                pl.indexOf('bthle') !== -1 || pl.indexOf('bluetooth') !== -1) {
                handleMap[hk(ret)] = p;
                if (pl.indexOf('hid') !== -1) {
                    emit('file', 'OPEN_HID handle=' + ret + ' path=' + p);
                }
                if (pl.indexOf('bthle') !== -1 || pl.indexOf('bluetooth') !== -1) {
                    emit('ble', 'OPEN_BLE handle=' + ret + ' path=' + p);
                }
            }
        }
    });
    info('[OK] CreateFileW');
})();

// ============================================================================
// 2. CloseHandle - clean up handle map entries
// ============================================================================
(function () {
    var fn = findExport('kernel32.dll', 'CloseHandle');
    if (!fn) return;
    Interceptor.attach(fn, {
        onEnter: function (a) {
            var k = hk(a[0]);
            if (handleMap[k]) delete handleMap[k];
        }
    });
})();

// ============================================================================
// 3. NtDeviceIoControlFile - EC reads/writes + catch-all
// ============================================================================
(function () {
    var fn = findExport('ntdll.dll', 'NtDeviceIoControlFile');
    if (!fn) { info('[WARN] NtDeviceIoControlFile not found'); return; }

    Interceptor.attach(fn, {
        onEnter: function (a) {
            this.h = a[0];
            this.ioctl = a[5].toUInt32();
            this.ib = a[6]; this.is_ = a[7].toUInt32();
            this.ob = a[8]; this.os_ = a[9].toUInt32();
            this.acpi = ((this.ioctl >>> 16) === 0x9C40);
            this.bth = ((this.ioctl >>> 16) === 0x0041); // FILE_DEVICE_BLUETOOTH
            // Also check by handle — BLE device paths tracked via CreateFileW
            var dp = handleMap[hk(this.h)];
            if (dp) {
                var dpl = dp.toLowerCase();
                if (dpl.indexOf('bthle') !== -1 || dpl.indexOf('bluetooth') !== -1)
                    this.bth = true;
            }
        },
        onLeave: function (ret) {
            if (!logAllIoctl && !this.acpi && !this.bth) return;

            var code = this.ioctl;
            var name;
            if (code === 0x9C40A488) name = 'ECREAD';
            else if (code === 0x9C40A48C) name = 'ECWRITE';
            else if (code === 0x9C40A504) name = 'CUSTOMCTL';
            else name = 'IOCTL_0x' + code.toString(16).toUpperCase();

            var cat = this.bth ? 'ble' : (this.acpi ? 'ec' : 'ioctl');
            var addrInfo = '';
            if (this.acpi && this.is_ >= 4 && !this.ib.isNull()) {
                var addr = this.ib.readU32();
                var regName = ecName(addr);
                addrInfo = ' addr=0x' + addr.toString(16).toUpperCase();
                if (regName) addrInfo += '(' + regName + ')';

                if (name === 'ECREAD' && EC_POLL_ADDRS[addr]) {
                    cat = 'ec_poll';
                }
                if (name === 'ECWRITE' && this.is_ >= 8) {
                    var val = this.ib.add(4).readU32() & 0xFF;
                    addrInfo += ' val=0x' + val.toString(16).toUpperCase() + '(' + val + ')';
                }
            }

            var iH = hex(this.ib, this.is_);
            var oH = hex(this.ob, this.os_);
            var dp = handleMap[hk(this.h)];

            emit(cat, name + addrInfo +
                ' | in(' + this.is_ + ')=[' + iH +
                '] out(' + this.os_ + ')=[' + oH +
                '] ret=' + ret.toUInt32() +
                (dp ? ' dev=' + dp : ''));
        }
    });
    info('[OK] NtDeviceIoControlFile');
})();

// ============================================================================
// 4. USB HID - SetFeature, GetFeature, SetOutputReport, GetAttributes
// ============================================================================
var hidHooked = {};

function hookHid(name, dir) {
    if (hidHooked[name]) return false;
    var fn = findExport('hid.dll', name);
    if (!fn) return false;
    hidHooked[name] = true;

    Interceptor.attach(fn, {
        onEnter: function (a) {
            this.h = a[0]; this.b = a[1]; this.l = a[2].toUInt32();
            this.fn_ = name;
            if (dir !== 'in') this.bh = hex(this.b, this.l);
        },
        onLeave: function (ret) {
            var bh = (dir === 'in') ? hex(this.b, this.l) : this.bh;
            var dp = handleMap[hk(this.h)] || '?';
            emit('hid', this.fn_ + ' | h=' + this.h + ' dev=' + dp +
                ' len=' + this.l + ' buf=[' + bh + '] ret=' + ret);
        }
    });
    info('[OK] ' + name);
    return true;
}

(function () {
    var anyFound = false;
    anyFound = hookHid('HidD_SetFeature', 'out') || anyFound;
    anyFound = hookHid('HidD_GetFeature', 'in') || anyFound;
    anyFound = hookHid('HidD_SetOutputReport', 'out') || anyFound;

    if (!anyFound) {
        info('[WARN] hid.dll not loaded yet - HID hooks will activate when it loads');
    }

    // HidD_GetAttributes - identify VID/PID of HID devices
    var ga = findExport('hid.dll', 'HidD_GetAttributes');
    if (ga && !hidHooked['HidD_GetAttributes']) {
        hidHooked['HidD_GetAttributes'] = true;
        Interceptor.attach(ga, {
            onEnter: function (a) { this.h = a[0]; this.ab = a[1]; },
            onLeave: function (ret) {
                if (ret.toUInt32() === 0) return;
                try {
                    var vid = this.ab.add(4).readU16();
                    var pid = this.ab.add(6).readU16();
                    var ver = this.ab.add(8).readU16();
                    var dp = handleMap[hk(this.h)] || '?';
                    emit('hid', 'HidD_GetAttributes | h=' + this.h +
                        ' VID=0x' + ('000' + vid.toString(16)).slice(-4).toUpperCase() +
                        ' PID=0x' + ('000' + pid.toString(16)).slice(-4).toUpperCase() +
                        ' Ver=' + ver + ' dev=' + dp);
                } catch (e) { }
            }
        });
        info('[OK] HidD_GetAttributes');
    }

    // HidP_GetCaps - report sizes
    var gc = findExport('hid.dll', 'HidP_GetCaps');
    if (gc && !hidHooked['HidP_GetCaps']) {
        hidHooked['HidP_GetCaps'] = true;
        Interceptor.attach(gc, {
            onEnter: function (a) { this.caps = a[1]; },
            onLeave: function (ret) {
                try {
                    var usage = this.caps.readU16();
                    var usagePage = this.caps.add(2).readU16();
                    var inputLen = this.caps.add(4).readU16();
                    var outputLen = this.caps.add(6).readU16();
                    var featureLen = this.caps.add(8).readU16();
                    emit('hid', 'HidP_GetCaps | Usage=0x' + usage.toString(16).toUpperCase() +
                        ' UsagePage=0x' + usagePage.toString(16).toUpperCase() +
                        ' InLen=' + inputLen + ' OutLen=' + outputLen +
                        ' FeatureLen=' + featureLen);
                } catch (e) { }
            }
        });
        info('[OK] HidP_GetCaps');
    }
})();

// ============================================================================
// 5. WriteFile - filtered for HID device handles
// ============================================================================
(function () {
    var fn = findExport('kernel32.dll', 'WriteFile');
    if (!fn) return;
    Interceptor.attach(fn, {
        onEnter: function (a) {
            this.h = a[0]; this.b = a[1]; this.l = a[2].toUInt32();
            var dp = handleMap[hk(this.h)] || '';
            this.isHid = dp.toLowerCase().indexOf('hid') !== -1;
            if (this.isHid) { this.bh = hex(this.b, this.l); this.dp = dp; }
        },
        onLeave: function (ret) {
            if (!this.isHid) return;
            emit('hid', 'WriteFile | h=' + this.h + ' dev=' + this.dp +
                ' len=' + this.l + ' buf=[' + this.bh + '] ret=' + ret);
        }
    });
    info('[OK] WriteFile (HID filter)');
})();

// ============================================================================
// 6. ReadFile - filtered for HID device handles
// ============================================================================
(function () {
    var fn = findExport('kernel32.dll', 'ReadFile');
    if (!fn) return;
    Interceptor.attach(fn, {
        onEnter: function (a) {
            this.h = a[0]; this.b = a[1]; this.l = a[2].toUInt32();
            var dp = handleMap[hk(this.h)] || '';
            this.isHid = dp.toLowerCase().indexOf('hid') !== -1;
            this.dp = dp;
        },
        onLeave: function (ret) {
            if (!this.isHid) return;
            var bh = hex(this.b, this.l);
            emit('hid', 'ReadFile | h=' + this.h + ' dev=' + this.dp +
                ' len=' + this.l + ' buf=[' + bh + '] ret=' + ret);
        }
    });
    info('[OK] ReadFile (HID filter)');
})();

// ============================================================================
// 7. DDC/CI - dxva2.dll (may not be loaded in GCUService)
// ============================================================================
(function () {
    var dxva2 = Process.findModuleByName('dxva2.dll');
    if (!dxva2) {
        info('[INFO] dxva2.dll not loaded - DDC/CI not used by this process');
        info('[INFO] Local dimming likely goes through WMI->ACPI, not DDC/CI');
        return;
    }

    var sv = findExport('dxva2.dll', 'SetVCPFeature');
    if (sv) {
        Interceptor.attach(sv, {
            onEnter: function (a) {
                this.m = a[0];
                this.c = a[1].toUInt32() & 0xFF;
                this.v = a[2].toUInt32();
            },
            onLeave: function (ret) {
                emit('ddc', 'SetVCPFeature | mon=' + this.m +
                    ' VCP=0x' + ('0' + this.c.toString(16)).slice(-2).toUpperCase() +
                    ' val=' + this.v + '(0x' + this.v.toString(16).toUpperCase() + ')' +
                    ' ret=' + ret);
            }
        });
        info('[OK] SetVCPFeature');
    }

    var gv = findExport('dxva2.dll', 'GetVCPFeatureAndVCPFeatureReply');
    if (gv) {
        Interceptor.attach(gv, {
            onEnter: function (a) {
                this.m = a[0]; this.c = a[1].toUInt32() & 0xFF;
                this.pCur = a[3]; this.pMax = a[4];
            },
            onLeave: function (ret) {
                var cur = 0, mx = 0;
                try { cur = this.pCur.readU32(); } catch (e) { }
                try { mx = this.pMax.readU32(); } catch (e) { }
                emit('ddc', 'GetVCPFeature | mon=' + this.m +
                    ' VCP=0x' + ('0' + this.c.toString(16)).slice(-2).toUpperCase() +
                    ' cur=' + cur + ' max=' + mx + ' ret=' + ret);
            }
        });
        info('[OK] GetVCPFeature');
    }
})();

// ============================================================================
// 8. WMI detection - CoCreateInstance for CLSID_WbemLocator
// ============================================================================
(function () {
    var cc = findExport('combase.dll', 'CoCreateInstance') ||
             findExport('ole32.dll', 'CoCreateInstance');
    if (!cc) { info('[WARN] CoCreateInstance not found'); return; }

    Interceptor.attach(cc, {
        onEnter: function (a) {
            try {
                // CLSID_WbemLocator = {4590f811-1d3a-11d0-891f-00aa004b2e24}
                var d1 = a[0].readU32();
                this.isWbem = (d1 === 0x4590f811);
            } catch (e) { this.isWbem = false; }
        },
        onLeave: function (ret) {
            if (this.isWbem) {
                emit('wmi', 'CoCreateInstance(CLSID_WbemLocator) detected - WMI session');
            }
        }
    });
    info('[OK] CoCreateInstance (WMI detection)');
})();

// ============================================================================
// 9. WMI module detection
// ============================================================================
(function () {
    var fastprox = Process.findModuleByName('fastprox.dll');
    if (fastprox) {
        info('[INFO] fastprox.dll loaded - WMI provider is in-process');
    }
    var wbemprox = Process.findModuleByName('wbemprox.dll');
    if (wbemprox) {
        info('[INFO] wbemprox.dll loaded');
    }
})();

// ============================================================================
// 10. BLE GATT - bluetoothapis.dll hooks
// ============================================================================
var bleHooked = false;
var bleCallbackHooked = {};

function hookBleApis() {
    if (bleHooked) return;
    var mod = Process.findModuleByName('bluetoothapis.dll');
    if (!mod) return;
    bleHooked = true;

    // -- BluetoothGATTSetCharacteristicValue (outgoing writes) --
    var setVal = null;
    try { setVal = mod.getExportByName('BluetoothGATTSetCharacteristicValue'); } catch (e) {}
    if (setVal) {
        Interceptor.attach(setVal, {
            onEnter: function (a) {
                this.h = a[0];
                this.charVal = a[2]; // BTH_LE_GATT_CHARACTERISTIC_VALUE*
            },
            onLeave: function (ret) {
                try {
                    var dataSize = this.charVal.readU32();
                    var dataPtr = this.charVal.add(4);
                    var dataHex = hex(dataPtr, dataSize);
                    emit('ble', 'BLE_GATT_WRITE | h=' + this.h +
                        ' len=' + dataSize + ' data=[' + dataHex +
                        '] ret=0x' + (ret.toUInt32() >>> 0).toString(16));
                } catch (e) {
                    emit('ble', 'BLE_GATT_WRITE | h=' + this.h +
                        ' (parse error: ' + e + ')');
                }
            }
        });
        info('[OK] BluetoothGATTSetCharacteristicValue (BLE writes)');
    }

    // -- BluetoothGATTGetCharacteristicValue (reads) --
    var getVal = null;
    try { getVal = mod.getExportByName('BluetoothGATTGetCharacteristicValue'); } catch (e) {}
    if (getVal) {
        Interceptor.attach(getVal, {
            onEnter: function (a) {
                this.h = a[0];
                this.charVal = a[3]; // output BTH_LE_GATT_CHARACTERISTIC_VALUE*
                this.bufSize = a[2].toUInt32();
            },
            onLeave: function (ret) {
                if (ret.toUInt32() !== 0) return; // S_OK only
                try {
                    var dataSize = this.charVal.readU32();
                    var dataPtr = this.charVal.add(4);
                    var dataHex = hex(dataPtr, dataSize);
                    emit('ble', 'BLE_GATT_READ | h=' + this.h +
                        ' len=' + dataSize + ' data=[' + dataHex + ']');
                } catch (e) {
                    emit('ble', 'BLE_GATT_READ | h=' + this.h +
                        ' (parse error: ' + e + ')');
                }
            }
        });
        info('[OK] BluetoothGATTGetCharacteristicValue (BLE reads)');
    }

    // -- BluetoothGATTRegisterEvent (notification subscription + callback hook) --
    var regEvt = null;
    try { regEvt = mod.getExportByName('BluetoothGATTRegisterEvent'); } catch (e) {}
    if (regEvt) {
        Interceptor.attach(regEvt, {
            onEnter: function (a) {
                this.h = a[0];
                this.eventType = a[1].toUInt32();
                this.callback = a[3]; // PFNBLUETOOTH_GATT_EVENT_CALLBACK
            },
            onLeave: function (ret) {
                emit('ble', 'BLE_GATT_REGISTER_EVENT | h=' + this.h +
                    ' type=' + this.eventType +
                    ' callback=' + this.callback +
                    ' ret=0x' + (ret.toUInt32() >>> 0).toString(16));

                if (ret.toUInt32() !== 0) return; // S_OK only
                var cbAddr = this.callback.toString();
                if (bleCallbackHooked[cbAddr]) return;
                bleCallbackHooked[cbAddr] = true;

                try {
                    Interceptor.attach(this.callback, {
                        onEnter: function (a) {
                            var eventType = a[0].toUInt32();
                            var eventParam = a[1];
                            try {
                                // BLUETOOTH_GATT_VALUE_CHANGED_EVENT (x64):
                                //   offset 0: USHORT ChangedAttributeHandle
                                //   offset 8: size_t CharacteristicValueDataSize
                                //   offset 16: PBTH_LE_GATT_CHARACTERISTIC_VALUE*
                                var attrHandle = eventParam.readU16();
                                var charValPtr = eventParam.add(16).readPointer();
                                var dataSize = charValPtr.readU32();
                                var dataPtr = charValPtr.add(4);
                                var dataHex = hex(dataPtr, dataSize);
                                emit('ble', 'BLE_GATT_NOTIFY | attr=0x' +
                                    attrHandle.toString(16).toUpperCase() +
                                    ' len=' + dataSize +
                                    ' data=[' + dataHex + ']');
                            } catch (e) {
                                emit('ble', 'BLE_GATT_NOTIFY | type=' +
                                    eventType + ' (parse error: ' + e + ')');
                            }
                        }
                    });
                    info('[OK] BLE notification callback hooked at ' + cbAddr);
                } catch (e) {
                    info('[WARN] Failed to hook BLE callback: ' + e);
                }
            }
        });
        info('[OK] BluetoothGATTRegisterEvent (BLE notifications)');
    }

    // -- BluetoothGATTGetCharacteristics (service enumeration) --
    var getChars = null;
    try { getChars = mod.getExportByName('BluetoothGATTGetCharacteristics'); } catch (e) {}
    if (getChars) {
        Interceptor.attach(getChars, {
            onEnter: function (a) { this.h = a[0]; },
            onLeave: function (ret) {
                emit('ble', 'BLE_GATT_GET_CHARS | h=' + this.h +
                    ' ret=0x' + (ret.toUInt32() >>> 0).toString(16));
            }
        });
        info('[OK] BluetoothGATTGetCharacteristics (BLE enumeration)');
    }

    // -- BluetoothGATTGetServices (service discovery) --
    var getSvcs = null;
    try { getSvcs = mod.getExportByName('BluetoothGATTGetServices'); } catch (e) {}
    if (getSvcs) {
        Interceptor.attach(getSvcs, {
            onEnter: function (a) { this.h = a[0]; },
            onLeave: function (ret) {
                emit('ble', 'BLE_GATT_GET_SERVICES | h=' + this.h +
                    ' ret=0x' + (ret.toUInt32() >>> 0).toString(16));
            }
        });
        info('[OK] BluetoothGATTGetServices (BLE discovery)');
    }

    if (!setVal && !getVal && !regEvt && !getChars && !getSvcs) {
        info('[WARN] bluetoothapis.dll loaded but no GATT exports found');
    }
}

// Try hooking now (may already be loaded)
hookBleApis();
if (!bleHooked) {
    info('[INFO] bluetoothapis.dll not loaded yet - BLE hooks will activate when it loads');
}

// ============================================================================
// 11. Module load monitor - detect late-loaded DLLs
// ============================================================================
var seenMods = {};
Process.enumerateModules().forEach(function (m) {
    seenMods[m.name.toLowerCase()] = true;
});

setInterval(function () {
    Process.enumerateModules().forEach(function (m) {
        var n = m.name.toLowerCase();
        if (seenMods[n]) return;
        seenMods[n] = true;
        info('[MODULE] Loaded: ' + m.name + ' at ' + m.base);

        if (n === 'hid.dll') {
            info('[INFO] hid.dll now loaded - hooking HID functions');
            hookHid('HidD_SetFeature', 'out');
            hookHid('HidD_GetFeature', 'in');
            hookHid('HidD_SetOutputReport', 'out');
        }
        if (n === 'bluetoothapis.dll') {
            info('[INFO] bluetoothapis.dll now loaded - hooking BLE GATT functions');
            hookBleApis();
        }
        if (n === 'dxva2.dll') {
            info('[INFO] dxva2.dll now loaded - restart script to hook DDC/CI');
        }
    });
}, 3000);

// ============================================================================
// 12. RPC exports - runtime configuration from Python
// ============================================================================
rpc.exports = {
    set_ioctl_mode: function (all) {
        logAllIoctl = all;
        info('[CONFIG] logAllIoctl = ' + logAllIoctl);
        return logAllIoctl;
    },
    get_modules: function () {
        return Process.enumerateModules().map(function (m) {
            return m.name + ' @ ' + m.base;
        });
    },
    get_acpi_handles: function () {
        var result = [];
        for (var k in handleMap) {
            var p = handleMap[k];
            if (p.toLowerCase().indexOf('acpi') !== -1) {
                result.push(p);
            }
        }
        return result;
    },
    ping: function () { return 'ok'; }
};

// ============================================================================
// READY
// ============================================================================
info('[READY] All hooks installed. Monitoring: EC, HID, DDC/CI, WMI, BLE.');
info('[NOTE] BLE hooks target bluetoothapis.dll GATT functions (write, read, notify).');
info('[NOTE] If BLE produces NO hooks, the process may not have Bluetooth access.');
