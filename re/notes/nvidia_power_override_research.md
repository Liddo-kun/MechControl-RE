# NVIDIA power/current overrides: mVolt investigation

Date: 2026-10-28. Scope: mVolt+ 0.50.8 static analysis and an authorized 180 W
live experiment. No MechControl feature implementation or deployment in this
investigation. Earlier uncommitted EC TGP-unlock changes were preserved.

Subsequent implementation and live verification are recorded in
`nvidia_power_integration.md`. The original 180 W mVolt override described below
has since been removed; final GPU limits were restored to stock after testing.

## Live result and machine state

Jon authorized setting only the power ceiling to 180 W and restarting the GPU.

- GPU: RTX 5090 Laptop, PCI 10DE:2C18, subsystem 6040:1D05.
- NVIDIA driver: 617.14; VBIOS: 98.03.5e.00.1e.
- Windows Test Mode was not enabled for this experiment. No custom GPU kernel
  driver was installed. Secure Boot was already off; this is not a Secure Boot test.
- Initial `mVolt --power-cap-watts 180` failed validation against the 175 W maximum.
- In mVolt Settings -> Advanced tuning -> BIOS override -> Direct -> Power &
  current, changed **Power limit only** from 175 to 180, then Save limits.
- Restart GPU succeeded. mVolt displayed "The new limits are active and verified."
- Immediately after restart, NVIDIA maximum was **180 W**, enforced limit **175 W**.
- Retried `--power-cap-watts 180`: mVolt returned `success:true`; independent
  `nvidia-smi` reported **maximum 180 W and enforced 180 W**. A later check retained both.
- NVVDD current/default/max stayed **210 A**; MSVDD stayed **60 A**. Voltage
  offsets, clocks and thermal-input overrides remained unchanged in status.
- **180 W remains applied at the end of the experiment.** The BIOS override is
  persistent; the lifetime of the live power request across reboot was not tested.
- No workload or external power measurement was performed. This proves driver
  limit acceptance, not actual sustained 180 W consumption. Jon's GPU is shunt-modded.

Recovery is available in mVolt's BIOS-override editor using **Use original limits /
Remove BIOS override**, followed by the offered GPU/Windows restart. Dashboard
Reset all does not remove the persistent override. Retain the recovery data:

`C:\ProgramData\mVolt+\NativeLimits\fcf4425c9f081d793e7235b12ae5beb3be8588cbba84bf9548ad6a3afc66fc9d\state.mvl`

## Software and evidence

- Public repository: https://github.com/b00nz/mVolt
- Source is not published there. Author says the NVAPI layer might be published
  separately: https://github.com/b00nz/mVolt/issues/9
- Examined executable: `C:\Temp\tandem\mVolt-0.50.8.exe`.
- SHA256: `E16D18BA26C1D12FD6CBB99D589CE1AB64338A3AF8544DE94DA3A7AD07C606C9`.
- Native x64, no CLR directory. The EXE itself is not Authenticode-signed; that
  is separate from kernel-driver signing.
- Ghidra 12.2 installed at `C:\Program_Files\Ghidra\ghidra_12.2_PUBLIC`.
- Required JDK 25 installed at `C:\Program_Files\Ghidra\Java\jdk-25.0.5+8`.
  Ghidra and JDK downloads were checked against their publishers' SHA256 values.
  Ghidra's launch.properties points JAVA_HOME_OVERRIDE at this JDK; the machine's
  default Java remains unchanged.
- Ghidra project: `C:\Temp\tandem\mvolt-ghidra\Mvolt0508.gpr`.
- Targeted pseudocode: `C:\Temp\tandem\mvolt-decompiled\`.
- Native-limit subsystem pseudocode: `C:\Temp\tandem\mvolt-native-limits\`.
- Export script: `C:\Temp\tandem\MvoltExport.java`.
- Before/staged/active GPU driver-key exports:
  `C:\Temp\tandem\mvolt-180w-driver-{before,staged,active}.reg`.
- Exact driver key: `C:\Temp\tandem\mvolt-180w-driver-key.txt`.
- Runtime results: `mvolt-before-180w.json`, `mvolt-after-180w.json`,
  `mvolt-apply-180w-after-override.txt`, `mvolt-status-180w-active.json` in the scratch directory.
- Screenshots: `mvolt-power-override-{before,edited,staged,active}.png` there.

These scratch files are local analysis artifacts, not application dependencies.

## Mechanism: two distinct layers

1. **Persistent ceiling/default changes:** the stock NVIDIA driver consumes
   software BIOS overrides stored in its per-adapter Windows driver registry key.
   This is not EC access, physical VBIOS flashing or the old XMGPowerPatch kernel
   memory patcher. Direct mode stores targeted records; BIOS-file mode stores a
   larger replacement input. This experiment used Direct mode only.
2. **Runtime operating request:** NVIDIA private control interfaces set a power
   request within the newly available ceiling. Changing the ceiling alone did
   not raise the enforced limit in this experiment. Runtime status afterwards
   showed source `F8 = 180000`, `FE = null`, enforced/max `180000`.

The executable uses NVAPI and an internal NVIDIA RM transport for some laptop
request/budget operations. This is more than an EC slider or ordinary `nvidia-smi -pl`.
Its optional PawnIO dependency is for CPU/RAM monitoring, not this registry override.

## Direct override record, confirmed by the experiment and decompilation

The only added registry value when staging 180 W was:

```
romOverride00 (REG_BINARY)
98 03 5E 00 1E 36 7C 05 00 04 20 BF 02 00
```

Layout:

| Offset | Size | Meaning in this record |
|---|---:|---|
| 0 | 5 | VBIOS version bytes: 98.03.5e.00.1e |
| 5 | 4 | Little-endian offset within the BIOS image: 0x57C36 |
| 9 | 1 | Replacement byte count: 4 |
| 10 | 4 | Replacement little-endian DWORD: 180000 mW |

`FUN_14029fc80` builds these records: five version bytes, four offset bytes,
one length byte, then replacement data. It checks original/target spans and
permits at most 256 records, each payload at most 0xF6 bytes.

`FUN_1402684e0` constructs registry names. Its inventory has `vbios`,
`EnableGpuFirmware`, `romOverride00` through `romOverrideFF`,
`RMHwSpeedoOverride`, and `FixBlackScreenIssue`. Only `romOverride00` changed
in this test; the other names are not instructions to modify them.

`FUN_1402696e0` uses SetupAPI to open the specific adapter's driver registry key
(`SetupDiOpenDevRegKey`, global scope, DIREG_DRV). `FUN_14026c730` writes/deletes
values with RegSetValueExW/RegDeleteValueW and flushes the key. The surrounding
code checks adapter identity, ownership/recovery data and concurrent changes.

## Original versus active BIOS input

mVolt's recovery record retained original and consumed BIOS images. Each extracted
image is 493824 bytes. A full byte comparison found only two changed bytes:

```
offset 0x57C36: 98 -> 20
offset 0x57C37: AB -> BF
```

The complete DWORD changed `98 AB 02 00` (175000) to `20 BF 02 00` (180000).

- Original: `C:\Temp\tandem\mvolt-5090-original.rom`
  SHA256 `844f373502806b774849f09f6bc48891fa8ec27dee1f7b7a28462726087f95f9`.
- Consumed: `C:\Temp\tandem\mvolt-5090-active-180w.rom`
  SHA256 `8bfdfb00cdfbc9ecf6c59ca3aafef031a53a27133ce347293d17c09237168605`.

## Discovering the fields without hardcoding this laptop

Ghidra identifies the parsing chain:

- `FUN_14028cad0`: validate PCI option-ROM images, PCIR/NPDS/RGIS/NPDE records,
  locate and validate the BIT directory, establish image-coordinate translation.
- `FUN_14028c970`: map logical BIOS offsets to the supplied image. Addresses beyond
  the legacy image include the intervening EFI image length when one exists.
- `FUN_140291fb0`: obtain the Power Policy table from BIT token **P + 0x2C**;
  validate its supported header/layout and return entries/stride.
- `FUN_140292520`: identify power/current-related fields, including the board
  power policy and optional BIOS-profile-dependent overrides.
- `FUN_140293950`: identify NVVDD/MSVDD current-limit fields and relevant profile
  variants, validating record types, bounds and monitor-channel identities.

For **this captured BIOS**, not universal constants:

| Item | Observed value |
|---|---|
| BIT directory | 0xDF0 |
| BIT P token data | 0xF36, version 2 |
| Logical policy table pointer at P+0x2C | 0x3FB1D |
| EFI image length added by coordinate translation | 0x18000 |
| Physical policy table | 0x57B1D |
| Policy header | version 0x40, header 0x38, entry stride 0x66, count 20 |
| Board-power entry | index 2, type 1, entry at 0x57C21 |
| Board-power min/default/max | +0x0D/+0x11/+0x15: 5000/95000/175000 mW |
| NVVDD entry | index 13, type 7, monitor channel 0x1B, at 0x58083 |
| NVVDD default/max DWORD offsets | 0x58094 / 0x58098, both 210000 |
| MSVDD entry | index 14, type 7, monitor channel 0x1C, at 0x580E9 |
| MSVDD default/max DWORD offsets | 0x580FA / 0x580FE, both 60000 |

mVolt labels the rail values as mA internally / A in the UI. Both current
default and maximum fields are involved in raising their initialized ranges,
unlike the board-power test which changed only the maximum. Current-limit
changes were **not** applied in this experiment. BIOS profile-specific copies
can add more affected fields; do not assume a universal one/two-DWORD patch.

## Heavyfail's newer work and runtime control

https://github.com/b00nz/mVolt/issues/80 documents the 621.01 change: current-policy
MAX/CURRENT can be reconstructed from DEFAULT, so raising MAX alone is insufficient.
He reports paired DEFAULT/MAX changes, restart when ranges change, and CURRENT-only
changes without restart inside the available range. His supplied interface IDs:

| Operation | NVAPI QueryInterface ID | RM method |
|---|---|---|
| Current-policy INFO | 0x67F31384 | 0x2080A618 |
| Current-policy GET | 0x8B3E7343 | 0x2080A61A |
| Current-policy SET | 0xAFFC2279 | 0x2080E61B |
| Current-policy STATUS | 0x31B7A4CD | 0x2080A619 |
| Power-policy INFO | 0x54924BF5 | 0x2080A630 |
| Power-source GET | 0x33AB0353 | 0x2080A632 |
| Power-source SET | 0x17695269 | 0x2080E633 |

Treat these as interface leads, not a complete ABI or authorization to reuse his
621.01 RVAs on another driver. mVolt decompiled functions `FUN_14023a880` and
`FUN_14023c710` independently show request selection, range validation, internal
RM requests and readback. The former includes source F7 and RM A632/E633. The
latter handles ordinary/F8 requests. Full wire-layout and dispatch recovery are
still required before implementing equivalent runtime writes in MechControl.

https://github.com/b00nz/mVolt/issues/86 is separate VMIN/VFE-expression work;
it is not needed for the power/current feature.

The board input current limit (a separate HW_THRESHOLD current policy) is covered in
`nvidia_board_input_current.md`.

## Integration implications

The persistent power-ceiling mechanism is now hardware-confirmed on one driver/
BIOS combination. A production implementation must discover the BIOS layout and
match the selected NVIDIA adapter rather than bind it to an EC project ID or
copy the offsets above into a machine profile.

Remaining engineering work before application integration:

1. Recover and validate the complete read/control interfaces, including getting
   the BIOS image and binding its identity to SetupAPI and NVIDIA handles.
2. Implement supported BIOS parsing and a target-specific patch plan. Preserve
   defaults and unrelated policies except where a requested rail change requires
   paired default/max updates. Refuse unsupported layouts rather than guessing.
3. Persist original registry input and restoration data before staging writes;
   detect mVolt/other-tool ownership and external changes. Verify after restart.
4. Coordinate the persistent ceiling, runtime request, EC TGP/DB and user profiles.
   In particular, demonstrate that an EC failsafe reduction still reduces the
   enforced NVIDIA limit after the new request channels have been used.
   **Answered 2026-10-29:** yes. With F8 left at 185 W, the EC minimum made firmware
   drop F7 to 95 W and NVIDIA enforced 95 W within 2 s. The app also sets F8 to the
   minimum while unlocked. See `nvidia_power_integration.md`.
5. Test rail-change readback and restoration separately from this 180 W test.
   Validate actual load behaviour independently of the reported ceiling and
   account for shunt correction when interpreting measured power.

No new test-mode/signing dependency is indicated for this demonstrated route.
This does not establish support for every NVIDIA driver, laptop or security configuration.
