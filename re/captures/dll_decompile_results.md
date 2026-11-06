# ACPIDriverDll.dll Decompilation Results

## Where the DLL was found

**Path:** `C:\Program Files\OEM\机械革命控制中心\AiStoneService\MyControlCenter\ACPIDriverDll.dll`

The entire Control Center service lives at:
`C:\Program Files\OEM\机械革命控制中心\AiStoneService\MyControlCenter\`

**File type:** Native x64 C++ DLL (PE32+ executable, NOT managed .NET)

**GCUService.exe** is a managed .NET exe in the same directory, but is heavily obfuscated
(Fody/Costura — all method bodies replaced with `throw new Exception("Runtime exception")`
at compile time, real bodies patched in at runtime by the module initializer).

## Key Finding: EC Write Buffer Format — CONFIRMED

```
Write buffer: 8 bytes total
  Offset 0: addr  (DWORD / int32, little-endian)
  Offset 4: data  (DWORD / int32, little-endian)

Read buffer:  4 bytes input, 4 bytes output
  Input:  addr  (DWORD / int32, little-endian)
  Output: value (low byte = register value, upper 3 bytes undefined)
```

**Your assumption was correct: the write buffer is `int addr` + `int data`, 8 bytes.**

## Native DLL Exports (ACPIDriverDll.dll)

The DLL exports 19 functions:

| Ordinal | Name         | RVA     |
|---------|--------------|---------|
| 1       | ReadCMOS     | 0x3250  |
| 2       | ReadEC       | 0x3410  |
| 3       | ReadIO       | 0x3950  |
| 4       | ReadIndexIO  | 0x3B10  |
| 5       | ReadMEMB     | 0x35D0  |
| 6       | ReadPCI      | 0x3790  |
| 7       | SMAPCTable   | 0x4230  |
| 8       | TempRead1    | 0x3CE0  |
| 9       | TempRead2    | 0x3DC0  |
| 10      | TempRead3    | 0x3EA0  |
| 11      | TempWrite1   | 0x3F80  |
| 12      | TempWrite2   | 0x4070  |
| 13      | TempWrite3   | 0x4160  |
| 14      | WriteCMOS    | 0x3330  |
| 15      | WriteEC      | 0x34F0  |
| 16      | WriteIO      | 0x3A30  |
| 17      | WriteIndexIO | 0x3BF0  |
| 18      | WriteMEMB    | 0x36B0  |
| 19      | WritePCI     | 0x3870  |

## Full Disassembled ReadEC Function

**Prototype:** `byte ReadEC(int addr)` (stdcall, returns byte in AL/EAX)

```asm
; === ReadEC (RVA 0x3410) ===
; Input:  ECX = register address (e.g., 0x783)
; Output: AL = register value (byte)

ReadEC:
  push     rbx
  sub      rsp, 0x50

  ; Stack cookie setup
  mov      rax, [rip + __security_cookie]
  xor      rax, rsp
  mov      [rsp + 0x48], rax

  ; Store addr in global buffer at offset 0
  xor      r9d, r9d
  mov      dword ptr [globalBuf+0], ecx          ; globalBuf[0..3] = addr

  ; CreateFileW(L"\\\\.\\ACPIDriver", GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)
  mov      qword ptr [rsp + 0x30], 0             ; hTemplate = NULL
  lea      rcx, [L"\\\\.\\ACPIDriver"]
  mov      dword ptr [rsp + 0x28], 0             ; dwFlagsAndAttributes = 0
  mov      edx, 0xC0000000                       ; GENERIC_READ | GENERIC_WRITE
  mov      dword ptr [rsp + 0x20], 3             ; OPEN_EXISTING
  lea      r8d, [r9 + 3]                         ; dwShareMode = 3 (READ|WRITE)
  call     [CreateFileW]

  mov      rbx, rax                              ; save handle
  cmp      rax, -1                               ; INVALID_HANDLE_VALUE?
  jne      .success

  ; Error path: MessageBox("INVALID_HANDLE_VALUE") and return 0
  xor      r9d, r9d
  lea      r8, [L"INVALID_HANDLE_VALUE"]
  xor      edx, edx
  xor      ecx, ecx
  call     [MessageBoxW]
  xor      eax, eax                              ; return 0
  ; ... cookie check, epilogue, ret

.success:
  ; DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECREAD, &globalBuf, 0x400000, &globalBuf, 0x400000, &bytesReturned, NULL)
  mov      qword ptr [rsp + 0x38], 0             ; lpOverlapped = NULL
  lea      rax, [rsp + 0x40]
  mov      qword ptr [rsp + 0x30], rax           ; lpBytesReturned = &local
  lea      r8, [globalBuf]                        ; lpInBuffer = &globalBuf
  mov      dword ptr [rsp + 0x28], 0x400000      ; nOutBufferSize
  mov      edx, 0x9C40A488                       ; IOCTL_GPD_ACPI_ECREAD
  mov      r9d, 0x400000                          ; nInBufferSize
  mov      qword ptr [rsp + 0x20], r8            ; lpOutBuffer = &globalBuf (SAME buffer!)
  mov      rcx, rbx                              ; hDevice
  call     [DeviceIoControl]

  ; CloseHandle
  mov      rcx, rbx
  call     [CloseHandle]

  ; Return low byte from global buffer
  movzx    eax, byte ptr [globalBuf+0]           ; return value = first byte of output
  ; ... cookie check, epilogue, ret
```

**Summary of ReadEC:**
1. Stores `addr` as DWORD at globalBuf[0]
2. Opens `\\.\ACPIDriver`
3. Calls DeviceIoControl with IOCTL 0x9C40A488, same buffer for in/out
4. Closes handle
5. Returns low byte of globalBuf[0] (the driver overwrites the addr with the result)

## Full Disassembled WriteEC Function

**Prototype:** `void WriteEC(int addr, int data)` (stdcall)

```asm
; === WriteEC (RVA 0x34F0) ===
; Input:  ECX = register address, EDX = value to write

WriteEC:
  push     rbx
  sub      rsp, 0x50

  ; Stack cookie setup
  mov      rax, [rip + __security_cookie]
  xor      rax, rsp
  mov      [rsp + 0x48], rax

  ; Store addr and data in global buffer
  xor      r9d, r9d
  mov      dword ptr [globalBuf+0], ecx          ; globalBuf[0..3] = addr
  mov      dword ptr [globalBuf+4], edx          ; globalBuf[4..7] = data   <-- THIS IS THE KEY

  ; CreateFileW(L"\\\\.\\ACPIDriver", GENERIC_READ|GENERIC_WRITE, ..., OPEN_EXISTING)
  lea      rcx, [L"\\\\.\\ACPIDriver"]
  mov      qword ptr [rsp + 0x30], 0
  mov      edx, 0xC0000000
  mov      dword ptr [rsp + 0x28], 0
  lea      r8d, [r9 + 3]
  mov      dword ptr [rsp + 0x20], 3
  call     [CreateFileW]

  mov      rbx, rax
  cmp      rax, -1
  jne      .success

  ; Error: MessageBox, return
  xor      r9d, r9d
  lea      r8, [L"INVALID_HANDLE_VALUE"]
  xor      edx, edx
  xor      ecx, ecx
  call     [MessageBoxW]
  ; ... cookie check, epilogue, ret (no return value)

.success:
  ; DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECWRITE, &globalBuf, 0x400000, &globalBuf, 0x400000, &bytesReturned, NULL)
  mov      qword ptr [rsp + 0x38], 0             ; lpOverlapped = NULL
  lea      rax, [rsp + 0x40]
  mov      qword ptr [rsp + 0x30], rax           ; lpBytesReturned
  lea      r8, [globalBuf]                        ; lpInBuffer = &globalBuf
  mov      dword ptr [rsp + 0x28], 0x400000      ; nOutBufferSize
  mov      edx, 0x9C40A48C                       ; IOCTL_GPD_ACPI_ECWRITE
  mov      r9d, 0x400000                          ; nInBufferSize
  mov      qword ptr [rsp + 0x20], r8            ; lpOutBuffer = &globalBuf
  mov      rcx, rbx
  call     [DeviceIoControl]

  ; CloseHandle
  mov      rcx, rbx
  call     [CloseHandle]
  ; ... cookie check, epilogue, ret (no return value)
```

**Summary of WriteEC:**
1. Stores `addr` as DWORD at globalBuf[0]
2. Stores `data` as DWORD at globalBuf[4]
3. Opens `\\.\ACPIDriver`
4. Calls DeviceIoControl with IOCTL 0x9C40A48C, same buffer for in/out
5. Closes handle (no return value)

## Proof: Buffer Address Verification

Using RIP-relative address calculation from the raw disassembly:

| What                  | Instruction Address | Displacement | Effective RVA |
|-----------------------|--------------------:|-------------:|--------------:|
| addr store (ReadEC)   | 0x3428 (+6=0x342E)  | 0x279872     | **0x27CCA0**  |
| addr store (WriteEC)  | 0x3508 (+6=0x350E)  | 0x279792     | **0x27CCA0**  |
| data store (WriteEC)  | 0x350E (+6=0x3514)  | 0x279790     | **0x27CCA4**  |
| buffer ptr (ReadEC)   | 0x34A2 (+7=0x34A9)  | 0x2797F7     | **0x27CCA0**  |
| buffer ptr (WriteEC)  | 0x3586 (+7=0x358D)  | 0x279713     | **0x27CCA0**  |
| result read (ReadEC)  | 0x34D3 (+7=0x34DA)  | 0x2797C6     | **0x27CCA0**  |

All addresses align perfectly:
- `globalBuf` starts at RVA 0x27CCA0 (in .data section)
- `addr` is at offset 0 (0x27CCA0)
- `data` is at offset 4 (0x27CCA4)
- The DeviceIoControl buffer pointer points to 0x27CCA0 (offset 0)

## Managed Code Context (GCUService.exe — MyECIO.AcpiCtrl)

The .NET wrapper class `MyECIO.AcpiCtrl` has these relevant signatures (bodies obfuscated):

```csharp
namespace MyECIO;

public class AcpiCtrl
{
    // P/Invoke to kernel32
    [DllImport("kernel32.dll")]
    public static extern bool DeviceIoControl(
        IntPtr handle, uint controlCode,
        IntPtr inBuffer, int inBufferSize,
        ref int outBuffer, int outBufferSize,
        out int bytesReturned, IntPtr overlapped);

    // P/Invoke to native DLL (only used for SMAPCTable)
    [DllImport("ACPIDriverDll.dll", CallingConvention = CallingConvention.StdCall)]
    public static extern IntPtr SMAPCTable(IntPtr arg0);

    // Public API — "ReadName" maps to IOCTL code (e.g., "ECRAM" -> ECREAD)
    public void Read(string ReadName, ushort Addr, ref byte Data);
    public void Read(uint ReadName, ushort Addr, ref byte Data);
    public void Write(string ReadName, ushort Addr, byte Data);

    // Internal — direct IOCTL wrappers
    private void ReadACPI(uint ioctrl, int addr, ref int data);
    private void ReadACPI(uint ioctrl, int addr, ref byte data);
    private void WriteACPI(uint ioctrl, int addr, int data);

    // Other useful methods
    public void ReadPciDword(byte Bus, byte Dev, byte Func, uint Offset, ref uint pData);
    public void CustomRW(string methodName, int input, ref byte data);
    public void ReadSmartApcTableByKey(byte Offset, ref byte[] pData);
    public void WriteSmartApcTableByKey(byte[] Data);
}
```

**Note:** The managed code ALSO declares DeviceIoControl from kernel32.dll, so the
`ReadACPI`/`WriteACPI` methods likely call DeviceIoControl directly from managed code
(not through the native DLL). The native DLL appears to be used primarily for
`SMAPCTable`. Both paths use the same IOCTL interface to the same kernel driver,
so the buffer format is identical.

## Complete IOCTL Code Table

All IOCTL codes from `MyECIO.AcpiCtrl` (GPD_TYPE = 40000):

| Name                          | Hex          | Decimal      |
|-------------------------------|--------------|--------------|
| IOCTL_GPD_READ_PORT_UCHAR     | 0x9C406400   | 2621465600   |
| IOCTL_GPD_READ_PORT_USHORT    | 0x9C406404   | 2621465604   |
| IOCTL_GPD_READ_PORT_ULONG     | 0x9C406408   | 2621465608   |
| IOCTL_GPD_WRITE_PORT_UCHAR    | 0x9C40A440   | 2621482048   |
| IOCTL_GPD_WRITE_PORT_USHORT   | 0x9C40A444   | 2621482052   |
| IOCTL_GPD_WRITE_PORT_ULONG    | 0x9C40A448   | 2621482056   |
| IOCTL_GPD_ACPI_CMREAD         | 0x9C40A480   | 2621482112   |
| IOCTL_GPD_ACPI_CMWRITE        | 0x9C40A484   | 2621482116   |
| **IOCTL_GPD_ACPI_ECREAD**     | **0x9C40A488** | 2621482120 |
| **IOCTL_GPD_ACPI_ECWRITE**    | **0x9C40A48C** | 2621482124 |
| IOCTL_GPD_ACPI_MMREADB        | 0x9C40A490   | 2621482128   |
| IOCTL_GPD_ACPI_MMREADD        | 0x9C40A494   | 2621482132   |
| IOCTL_GPD_ACPI_MMWRITEB       | 0x9C40A498   | 2621482136   |
| IOCTL_GPD_ACPI_MMWRITED       | 0x9C40A49C   | 2621482140   |
| IOCTL_GPD_ACPI_PEREAD         | 0x9C40A4A0   | 2621482144   |
| IOCTL_GPD_ACPI_PEWRITE        | 0x9C40A4A4   | 2621482148   |
| IOCTL_GPD_ACPI_IOREAD         | 0x9C40A4C0   | 2621482176   |
| IOCTL_GPD_ACPI_IOWRITE        | 0x9C40A4C4   | 2621482180   |
| IOCTL_GPD_ACPI_INIOREAD       | 0x9C40A4C8   | 2621482184   |
| IOCTL_GPD_ACPI_INIOWRITE      | 0x9C40A4CC   | 2621482188   |
| IOCTL_GPD_ACPI_TMPREAD1       | 0x9C40A4D0   | 2621482192   |
| IOCTL_GPD_ACPI_TMPREAD2       | 0x9C40A4D4   | 2621482196   |
| IOCTL_GPD_ACPI_TMPREAD3       | 0x9C40A4D8   | 2621482200   |
| IOCTL_GPD_ACPI_TMPWRITE1      | 0x9C40A4DC   | 2621482204   |
| IOCTL_GPD_ACPI_TMPWRITE2      | 0x9C40A4E0   | 2621482208   |
| IOCTL_GPD_ACPI_TMPWRITE3      | 0x9C40A4E4   | 2621482212   |
| IOCTL_GPD_ACPI_SMAPCTABLE     | 0x9C40A500   | 2621482240   |
| IOCTL_GPD_ACPI_CUSTOMCTL      | 0x9C40A504   | 2621482244   |

## Other Constants

```csharp
public const byte SMRW_CMD_READ = 0xBB;   // 187
public const byte SMRW_CMD_WRITE = 0xAA;  // 170
```

## Additional Notes

1. **Device path:** `\\.\ACPIDriver` (wide string, confirmed from binary at RVA 0x2329C0)
2. **Error handling:** On CreateFile failure, shows MessageBox with "INVALID_HANDLE_VALUE"
3. **Handle lifecycle:** Opens and closes the device handle for EVERY read/write operation
   (no persistent handle). This means the driver must be okay with rapid open/close cycles.
4. **Buffer size passed to DeviceIoControl:** 0x400000 (4MB) — this is a large shared global
   buffer. The driver only reads/writes the first 4-8 bytes; the oversized parameter is harmless.
5. **Same buffer for in/out:** Both ReadEC and WriteEC pass the same pointer for lpInBuffer
   and lpOutBuffer. With METHOD_BUFFERED, the kernel copies input to SystemBuffer, the driver
   processes it, then the kernel copies SystemBuffer back to the output pointer.
6. **The managed code also has WMI-based EC access:** `WMIReadECRAM(ulong Addr, ref object data)`
   and `WMIWriteECRAM(ulong Addr, ulong Value)` exist as alternative paths, likely going through
   WMI/ACPI instead of the direct driver IOCTL.
7. **The WriteCMOS/ReadCMOS functions follow the EXACT same pattern** as WriteEC/ReadEC
   (identical structure, just different IOCTL codes: 0x9C40A480/0x9C40A484).

## How to use this for EC writes (C# pseudocode)

```csharp
// EC Read
byte ReadEC(ushort addr)
{
    var handle = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    int inBuf = addr;
    int outBuf = 0;
    uint bytesReturned;
    DeviceIoControl(handle, 0x9C40A488, ref inBuf, 4, ref outBuf, 4, out bytesReturned, IntPtr.Zero);
    CloseHandle(handle);
    return (byte)(outBuf & 0xFF);
}

// EC Write
void WriteEC(ushort addr, byte value)
{
    var handle = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    int[] buf = new int[] { addr, value };  // 8 bytes: [addr][data]
    int outBuf = 0;
    uint bytesReturned;
    DeviceIoControl(handle, 0x9C40A48C, buf, 8, ref outBuf, 4, out bytesReturned, IntPtr.Zero);
    CloseHandle(handle);
}
```

Or using IntPtr with Marshal for exact control:

```csharp
void WriteEC(ushort addr, byte value)
{
    var handle = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    IntPtr buf = Marshal.AllocHGlobal(8);
    Marshal.WriteInt32(buf, 0, addr);      // offset 0: address
    Marshal.WriteInt32(buf, 4, value);     // offset 4: data
    int bytesReturned;
    DeviceIoControl(handle, 0x9C40A48C, buf, 8, buf, 8, out bytesReturned, IntPtr.Zero);
    Marshal.FreeHGlobal(buf);
    CloseHandle(handle);
}
```
