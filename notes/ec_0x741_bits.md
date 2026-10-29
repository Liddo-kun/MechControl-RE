# EC 0x741 static investigation

## Principal result

Bit 7's exact meaning remains unproven. A significant new finding is that the recoverable OEM fan-alert implementation checks **bit 5 (0x20), not bit 7**. Therefore ADDR_FAN_ALERT_BYTE is not evidence that bit 7 means fan failure.

This was proved by statically decrypting ConfuserEx-protected GCUService.exe from official Eluktronics ControlCenter 4.23.47.11. This is an older Uniwill software lineage, not proof that every later model uses the same bit assignment. The newer 5.23.51.56 package for HYDROC-16 G2 (275HX/5080/5090) was also obtained, but its protected GCUService method bodies were not recovered.

## Recovered code

`fan-decrypted.cs:15-22`, also `service-decrypted.cs:37560-37567`:

```csharp
public int GetECFanAlert()
{
    byte Data = 0;
    EcCtrl.Read(GetType().Name, 1857, ref Data);
    BitArray bitArray = new BitArray(new byte[1] { Data });
    fanerror = Convert.ToInt32(bitArray[5]);
    return fanerror;
}
```

BitArray indexes least-significant bit first. 1857 decimal is 0x741. An input of 0x81 yields FanErrorStatus=0 in this implementation.

`service-decrypted.cs:36175-36180` and `46284-46289` construct `{ FanErrorStatus = _FanFunctionErrorInfo.GetECFanAlert() }`. Lines 36104-36107 and 46144-46147 send it using `m_View.SendStatusToClient("System/FanErrorInfo", funError)`. These functions are called by the respective Enable() methods at lines 36058 and 45896. This proves a client-facing fan-error status message; it does not independently prove the UI displays a notification/dialog.

The older fully recovered service has only three functional direct accesses to address 1857: activation RMW, keyboard-on-key RMW, and the bit-5 alert read. No direct bit-7 consumer or setter was found there. This is bounded evidence, not a proof about indirect accesses, firmware, or later protected versions.

## Other bits and setters

The newer official package's unprotected `default2025.cs:13945-13951` retains Set_APExistToEC:

```csharp
Read(GetType().Name, 1857, ref Data);
byte b = (byte)Convert.ToUInt64(Data);
b = ((!bExist) ? ((byte)(b & 0xFE)) : ((byte)(b | 1)));
Write(GetType().Name, 1857, b);
```

This explicitly sets/clears bit 0 while preserving bit 7. It contradicts any generalized claim that the OEM AP-existence setter necessarily clears the entire byte on disconnect. Repository equivalent: `decompilation/defaulttool_decompile.txt:13444-13450`; ACPI names bit 0 APPE.

`default2025.cs:5559-5572` Enable_EC_OnkeyPressed sets `b |= 8` for effects 6,14,17,4 with direction 1. Repository equivalent at `decompilation/defaulttool_decompile.txt:5403` onward. Thus bit 3 is a keyboard-lighting on-key-pressed enable.

Bit 2: the supplied measurements show EC clears it after 0x05 initialization; an exact firmware semantic was not recovered.

Bit 5: fan-alert status in the older recovered service, high confidence for that binary, provisional for IDY_7Y.

Bits 1,4,6,7: no semantic assignment recovered here.

## What can be said about bit 7's lifecycle

The user's supplied observations show it is clear on the first start after reboot, appears later in the boot, survives app stop and fan spin-up, and is restored around 190 ms after a whole-byte 0x05 write clears it. This is strong evidence for a firmware-maintained/reasserted condition, not merely a host-owned flag. The exact set condition, underlying latch reset condition, and whether reboot is the only durable reset are unknown. Restoring the byte after a write does not alone distinguish a latched event from a continually true condition. No additional hardware tests were performed.

## Binaries and provenance

All paths below are relative to `C:\Temp\tandem\astra-741`.

- `eluk.zip`: official Eluktronics-hosted package, downloaded from https://store-g9br3.mybigcommerce.com/content/Drivers/Control%20Center/ControlCenter_4.23.47.11_Eluktronics_v1.zip . SHA256 `7D6DABEA2D45DC339EA4C36C56327A2DCAEC4716411C88E1AA02F5AE22D2889D`.
- Original analyzed older service: `eluk-unpacked/app/UniwillService/MyControlCenter/GCUService.exe`, SHA256 `D067AF040969F333DB486AEDDB113CC1487B36F550DB7F83A7A5B818D5815E9C`.
- `eluk2025.zip`: ControlCenter_5.23.51.56_Eluktronics_V4, linked by the official https://www.eluktronics.com/hydroc-16-g2-drivers/ page, item 18, dated 9/24/2025. Saved page `eluk-page.html:1962` records Google Drive ID `1V2QOEU8JiC4yTUSm3OQxXdjlv3bMV6eb`. Direct download https://drive.usercontent.google.com/download?id=1V2QOEU8JiC4yTUSm3OQxXdjlv3bMV6eb&export=download&confirm=t . SHA256 `7DE8AE176EC42336A10510F28DB150D94A1C95155949487401A4DDDBC6EDB7E5`.
- Newer service: `eluk2025-unpacked/{app}/AiStoneService/MyControlCenter/GCUService.exe`, SHA256 `E5D388B45515D06567C2E71E4C2B882A1FA9313704D70785F19AEC960E361003`.
- Newer unprotected DefaultTool and GCUUtil decompiled to `default2025.cs` and `util2025.cs`. No further 0x741 bit-7 evidence found.

Recursive filename searches over C: and D: found repository decompilations and one ACPIDriverDll at `C:\Users\Jon\Downloads\Unbound-v1.0.2\Test\UCU-Release\ACPIDriverDll.dll`, but no surviving original GCUService/DefaultTool/GCUBridge binaries. Searches included D:\BU. One combined search timed out; subsequent searches completed the relevant remaining filename searches, though archives were not exhaustively unpacked.

MECHREVO official 2026 CCX package was located at https://driver.mechrevo.com/d.mechrevo.com/driver/MECHREVO2026/CCX/ControlCenterX_5.56.60.17_Mechrevo.zip . CLI download attempts returned ~10KB anti-bot HTML rather than ZIP. Browser fetch exceeded its 50MiB document limit. Thus `ccx*.zip` in this scratch directory are failed HTML downloads, not usable packages.

## Tools and static-only workflow

- Existing Python 3.13, PowerShell, curl, 7-Zip 26.00, ILSpyCmd 9.1.0.7988.
- innoextract 1.9 from https://constexpr.org/innoextract/files/innoextract-1.9-windows.zip extracted the older installer without execution.
- Newer Inno installer extracted using innounp from https://raw.githubusercontent.com/jrathlev/InnoUnpacker-Windows-GUI/refs/heads/master/innounp-2/bin/innounp-2.zip . innoextract 1.9 could not parse its 6.3 headers.
- `decrypt.py` is a purpose-built static ConfuserEx section decoder. It transcribes the antitamper algorithm and constants visible in `module.il:108-635`, reads PE bytes, and writes `GCUService.decrypted.exe`. It does not load or execute any OEM code. ILSpy then recovered `fan-decrypted.cs` and `service-decrypted.cs`.
- NETReactorSlayer source downloaded from https://github.com/SychicBoy/NETReactorSlayer/archive/refs/heads/master.zip . Before building, Context.cs was patched to disable reflection loading completely (including removing Assembly.Load/UnsafeLoadFrom), and target changed to net8.0. Built with existing C:\Users\Jon\.dotnet SDK 8.0.423. This static-only version failed to identify the newer encrypted methods (`slayer2025.log`); its output is not a successful method recovery and the protector identity is not established by that attempt.
- curl_cffi installed into scratch pydeps for a browser-TLS download attempt; it also received the MECHREVO anti-bot response.

No OEM executable was run or installed. No EC reads/writes, process/task changes, reboot, repository changes, or deployed-app changes were made by this investigation.

## Report wording recommendation

“The meaning of 0x741 bit 7 remains unknown. It behaves as a firmware-maintained condition that persists within a boot and is reasserted after host clearing. The register's ADDR_FAN_ALERT_BYTE alias must not be equated with bit 7: a statically recovered Uniwill OEM implementation uses bit 5 for FanErrorStatus and sends that status over System/FanErrorInfo. Confirmation of that mapping on the current IDY_7Y OEM build and identification of bit 7 require further protected-code or EC-firmware analysis.”
