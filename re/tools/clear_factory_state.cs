using System;
using System.IO;
using System.Runtime.InteropServices;

/// <summary>
/// Resets MechControl-managed state (EC registers + UEFI NVRAM flags) back to
/// the cold-boot / uninitialized state so that a full startup-sequence test
/// can be run without actually rebooting.
///
/// Mirrors exactly what MechControl's refactored startup writes:
///   - Features/EcActivationControl.InitializeEc()  (EC register init sequence)
///   - Features/UefiSetup.EnsureFlags()              (UEFI NVRAM flags)
///
/// Usage (admin):
///   clear_factory_state.exe            Clear EC init regs + UEFI flags (keep fan table, user profiles)
///   clear_factory_state.exe --check    Read-only: show current values
///   clear_factory_state.exe --ec-only  Clear EC registers only
///   clear_factory_state.exe --uefi-only Clear UEFI flags only
///   clear_factory_state.exe --fan-table Also fill 0xF00-0xF5F with 0xFF (simulates cold-boot fan table)
///   clear_factory_state.exe --wipe     Also delete userprofile_*.json + settings.json + mechcontrol.log
///   clear_factory_state.exe --all      Shortcut for: EC + UEFI + --fan-table + --wipe (full nuke)
///
/// Compile: csc.exe -nologo clear_factory_state.cs
///
/// SAFETY: Clearing 0x741/0x7C5/0x7C6 will drop the EC into failsafe
/// (fans ~26%, CPU possibly locked at 800MHz). Launch MechControl immediately
/// after running this tool so the init sequence re-arms the EC.
/// </summary>
class ClearFactoryState
{
    // ---------- EC driver P/Invoke ----------
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string path, uint access, uint share,
        IntPtr sec, uint disp, uint flags, IntPtr templ);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr dev, uint code,
        int[] inBuf, int inSize, ref int outBuf, int outSize,
        out int bytes, IntPtr overlapped);

    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    const uint IOCTL_ECREAD  = 0x9C40A488;
    const uint IOCTL_ECWRITE = 0x9C40A48C;

    // ---------- UEFI P/Invoke ----------
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint GetFirmwareEnvironmentVariableW(
        string lpName, string lpGuid, IntPtr pBuffer, uint nSize);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetFirmwareEnvironmentVariableW(
        string lpName, string lpGuid, IntPtr pValue, uint nSize);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool LookupPrivilegeValue(string sys, string name, out LUID luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr tok, bool disableAll,
        ref TOKEN_PRIVILEGES tp, uint len, IntPtr prev, IntPtr retLen);

    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();

    [StructLayout(LayoutKind.Sequential)]
    struct LUID { public uint LowPart; public int HighPart; }
    [StructLayout(LayoutKind.Sequential)]
    struct LUID_AND_ATTRIBUTES { public LUID Luid; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential)]
    struct TOKEN_PRIVILEGES { public uint PrivilegeCount; public LUID_AND_ATTRIBUTES Privileges; }

    const string OEM_GUID = "{9f33f85c-13ca-4fd1-9c4a-96217722c593}";
    const string VAR_NAME = "OemMagicVariable";
    const int OFF_POWER_MODE    = 0x2F;
    const int OFF_MEM_OC_SWITCH = 0x33;
    const int OFF_AP_EXIST      = 0x5D;
    const int OFF_AP_USE        = 0x61;

    // ---------- EC register targets ----------
    // Each entry corresponds to one step in EcActivationControl.InitializeEc().
    // Action kinds:
    //   "set"  - write a fixed byte (full-byte register)
    //   "andc" - read-modify-write: clear the bits in Mask (for shared bitfield regs)
    struct EcTarget
    {
        public int    Addr;
        public string Name;
        public string Kind;   // "set" or "andc"
        public byte   Value;  // for "set"
        public byte   Mask;   // for "andc" (bits to clear)
    }

    struct UefiFix
    {
        public int    Off;
        public string Name;
        public byte   Target;
    }

    // Registers reset to match what EcActivationControl.InitializeEc() writes.
    // See MechControl/Features/EcActivationControl.cs for the init sequence.
    static readonly EcTarget[] EC_TARGETS = new EcTarget[]
    {
        new EcTarget { Addr = 0x709, Name = "Handshake1 (OEM-present step 1)", Kind = "set",  Value = 0x00 },
        new EcTarget { Addr = 0x708, Name = "Handshake2 (OEM-present step 2)", Kind = "set",  Value = 0x00 },
        new EcTarget { Addr = 0x741, Name = "Activation (EC unlock byte)",    Kind = "set",  Value = 0x00 },
        new EcTarget { Addr = 0x748, Name = "FeatureEnable",                  Kind = "set",  Value = 0x00 },
        new EcTarget { Addr = 0x726, Name = "PowerMgmt byte9 (clear bit 7)",  Kind = "andc", Mask  = 0x80 },
        new EcTarget { Addr = 0x727, Name = "PowerMgmt CUME (clear bit 6)",   Kind = "andc", Mask  = 0x40 },
        // 0x7C5: clear ALL bits. EcActivationControl sets bits 5&6 (OEM init);
        // bit 7 (custom fan curve enable) is managed by FanControl. A true
        // cold-boot test wants both cleared so the startup snapshot sees an
        // uninitialized fan curve.
        new EcTarget { Addr = 0x7C5, Name = "FanCurveEnable (all bits)",      Kind = "set",  Value = 0x00 },
        new EcTarget { Addr = 0x7C6, Name = "FanCurveMode flag",              Kind = "set",  Value = 0x00 },
        new EcTarget { Addr = 0x7C7, Name = "FanCurveParam",                  Kind = "set",  Value = 0x00 },
        new EcTarget { Addr = 0x706, Name = "BIOS_CTRL (init-complete)",      Kind = "set",  Value = 0x00 },
    };

    static bool EnablePriv()
    {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x0028, out tok)) return false;
        LUID luid;
        if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid)) return false;
        TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
        tp.PrivilegeCount = 1;
        tp.Privileges.Luid = luid;
        tp.Privileges.Attributes = 2;
        AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
        return Marshal.GetLastWin32Error() == 0;
    }

    static IntPtr OpenEc()
    {
        IntPtr h = CreateFile(@"\\.\ACPIDriver", 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == new IntPtr(-1))
        {
            Console.WriteLine("FAILED to open \\\\.\\ACPIDriver. Run as admin; UWACPIDriver must be loaded.");
            return IntPtr.Zero;
        }
        return h;
    }

    static byte EcRead(IntPtr h, int addr)
    {
        int outBuf = 0;
        int bytes;
        DeviceIoControl(h, IOCTL_ECREAD, new int[] { addr }, 4, ref outBuf, 4, out bytes, IntPtr.Zero);
        return (byte)(outBuf & 0xFF);
    }

    static bool EcWrite(IntPtr h, int addr, byte val)
    {
        int[] inBuf = { addr, val };
        int dummy = 0;
        int bytes;
        return DeviceIoControl(h, IOCTL_ECWRITE, inBuf, 8, ref dummy, 4, out bytes, IntPtr.Zero);
    }

    static int ClearEc(bool readOnly)
    {
        Console.WriteLine();
        Console.WriteLine("=== EC registers (mirrors EcActivationControl.InitializeEc) ===");
        IntPtr h = OpenEc();
        if (h == IntPtr.Zero) return 1;

        int failures = 0;
        try
        {
            foreach (var t in EC_TARGETS)
            {
                byte before = EcRead(h, t.Addr);
                byte desired;
                string action;
                if (t.Kind == "set")
                {
                    desired = t.Value;
                    action  = "=0x" + desired.ToString("X2");
                }
                else // andc
                {
                    desired = (byte)(before & ~t.Mask);
                    action  = "&=0x" + ((byte)~t.Mask).ToString("X2");
                }

                if (before == desired)
                {
                    Console.WriteLine("  [0x{0:X3}] 0x{1:X2}              already cleared   ({2})",
                        t.Addr, before, t.Name);
                    continue;
                }

                if (readOnly)
                {
                    Console.WriteLine("  [0x{0:X3}] 0x{1:X2} -> 0x{2:X2} {3,-14} NEEDS CLEAR      ({4})",
                        t.Addr, before, desired, action, t.Name);
                    continue;
                }

                bool ok = EcWrite(h, t.Addr, desired);
                byte after = EcRead(h, t.Addr);
                string status;
                if (!ok)         { status = "WRITE FAILED"; failures++; }
                else if (after != desired)
                {
                    // 0x741 transforms from 0x05 -> 0x01 on EC activation; the
                    // reverse (clearing to 0x00) is accepted by the EC firmware
                    // but we'd rather flag any unexpected readback mismatch.
                    status = "MISMATCH after=0x" + after.ToString("X2");
                    failures++;
                }
                else status = "OK";

                Console.WriteLine("  [0x{0:X3}] 0x{1:X2} -> 0x{2:X2} {3,-14} {4,-12} ({5})",
                    t.Addr, before, after, action, status, t.Name);
            }
        }
        finally { CloseHandle(h); }

        Console.WriteLine(readOnly
            ? "(read-only mode, no writes performed)"
            : failures == 0 ? "EC clear: all registers OK"
                            : "EC clear: " + failures + " failure(s)");
        return failures;
    }

    static int ClearUefi(bool readOnly)
    {
        Console.WriteLine();
        Console.WriteLine("=== UEFI OemMagicVariable (mirrors UefiSetup.EnsureFlags) ===");

        if (!EnablePriv())
        {
            Console.WriteLine("FAILED to enable SeSystemEnvironmentPrivilege. Run as admin.");
            return 1;
        }

        IntPtr buf = Marshal.AllocHGlobal(4096);
        uint size = GetFirmwareEnvironmentVariableW(VAR_NAME, OEM_GUID, buf, 4096);
        if (size == 0)
        {
            Marshal.FreeHGlobal(buf);
            Console.WriteLine("  OemMagicVariable not found (err=" + Marshal.GetLastWin32Error() +
                "). Non-Uniwill BIOS? Nothing to clear.");
            return 0;
        }
        byte[] data = new byte[size];
        Marshal.Copy(buf, data, 0, (int)size);
        Marshal.FreeHGlobal(buf);

        if (size != 180)
        {
            Console.WriteLine("  Unexpected size " + size + " bytes (expected 180) — refusing to write.");
            return 1;
        }

        // "Cleared" = PowerMode=1 (Balance), all feature flags 0.
        // This matches the --clear mode in tools/uefi_fix.cs.
        UefiFix[] fixes = new UefiFix[]
        {
            new UefiFix { Off = OFF_POWER_MODE,    Name = "PowerMode",             Target = 1 },
            new UefiFix { Off = OFF_MEM_OC_SWITCH, Name = "MemoryOverClockSwitch", Target = 0 },
            new UefiFix { Off = OFF_AP_EXIST,      Name = "ApExistFlag",           Target = 0 },
            new UefiFix { Off = OFF_AP_USE,        Name = "ApUseFlag",             Target = 0 },
        };

        bool anyChange = false;
        foreach (UefiFix f in fixes)
        {
            byte cur = data[f.Off];
            string tag = cur == f.Target ? "already cleared" : "NEEDS CLEAR";
            Console.WriteLine("  [0x{0:X2}] {1,-24} {2} -> {3}   {4}",
                f.Off, f.Name, cur, f.Target, tag);
            if (cur != f.Target) { data[f.Off] = f.Target; anyChange = true; }
        }

        if (!anyChange)
        {
            Console.WriteLine("UEFI clear: nothing to do (all flags already cleared)");
            return 0;
        }
        if (readOnly)
        {
            Console.WriteLine("(read-only mode, no writes performed)");
            return 0;
        }

        IntPtr wBuf = Marshal.AllocHGlobal((int)size);
        Marshal.Copy(data, 0, wBuf, (int)size);
        bool ok = SetFirmwareEnvironmentVariableW(VAR_NAME, OEM_GUID, wBuf, size);
        Marshal.FreeHGlobal(wBuf);

        if (!ok)
        {
            Console.WriteLine("UEFI clear: write FAILED (err=" + Marshal.GetLastWin32Error() + ")");
            return 1;
        }

        Console.WriteLine("UEFI clear: written. Note — effective only on next reboot.");
        return 0;
    }

    static int ClearFanTable(bool readOnly)
    {
        // Fill 0xF00-0xF5F (96 bytes) with 0xFF. 0xFF is the "uninitialized"
        // marker that FanSnapshot.IsValid() rejects and DetectGpuPresent()
        // treats as "absent" — simulates a true cold-boot fan table.
        Console.WriteLine();
        Console.WriteLine("=== Fan table 0xF00-0xF5F (96 bytes -> 0xFF = uninitialized) ===");
        IntPtr h = OpenEc();
        if (h == IntPtr.Zero) return 1;

        int written = 0, skipped = 0, failures = 0;
        try
        {
            for (int addr = 0xF00; addr <= 0xF5F; addr++)
            {
                byte before = EcRead(h, addr);
                if (before == 0xFF) { skipped++; continue; }
                if (readOnly) { skipped++; continue; }
                if (!EcWrite(h, addr, 0xFF)) { failures++; continue; }
                byte after = EcRead(h, addr);
                if (after != 0xFF) failures++;
                else written++;
            }
        }
        finally { CloseHandle(h); }

        if (readOnly)
            Console.WriteLine("  " + (96 - skipped) + " byte(s) would be written (" + skipped + " already 0xFF)");
        else
            Console.WriteLine("  wrote " + written + ", already 0xFF " + skipped + ", failures " + failures);
        return failures;
    }

    static int WipeAppState()
    {
        Console.WriteLine();
        Console.WriteLine("=== MechControl app state (userprofile_*.json, settings.json, mechcontrol.log) ===");

        // Scan the usual candidate locations the app writes to:
        //  - C:\Program Files\MechControl\        (installed location)
        //  - MechControl\bin\Release\net8.0-windows\  (normal build output)
        //  - MechControl\bin\Debug\net8.0-windows\    (debug build)
        string toolDir = AppDomain.CurrentDomain.BaseDirectory;
        string repoRoot = Path.GetFullPath(Path.Combine(toolDir, ".."));
        string[] dirs = new string[]
        {
            @"C:\Program Files\MechControl",
            Path.Combine(repoRoot, "MechControl", "bin", "Release", "net8.0-windows"),
            Path.Combine(repoRoot, "MechControl", "bin", "Debug",   "net8.0-windows"),
        };

        int deleted = 0;
        foreach (string dir in dirs)
        {
            if (!Directory.Exists(dir)) continue;
            Console.WriteLine("  " + dir);
            foreach (string name in new[] {
                "settings.json", "mechcontrol.log",
                "userprofile_1.json", "userprofile_2.json", "userprofile_3.json",
                "userprofile_4.json", "userprofile_5.json" })
            {
                string path = Path.Combine(dir, name);
                if (File.Exists(path))
                {
                    try { File.Delete(path); deleted++; Console.WriteLine("    deleted " + name); }
                    catch (Exception ex) { Console.WriteLine("    FAILED " + name + ": " + ex.Message); }
                }
            }
        }
        Console.WriteLine("Wipe: " + deleted + " file(s) deleted");
        return 0;
    }

    static void Main(string[] args)
    {
        bool readOnly = false, ecOnly = false, uefiOnly = false, wipe = false, fanTable = false;
        foreach (string a in args)
        {
            if (a == "--check")     readOnly = true;
            else if (a == "--ec-only")   ecOnly = true;
            else if (a == "--uefi-only") uefiOnly = true;
            else if (a == "--wipe")      wipe = true;
            else if (a == "--fan-table") fanTable = true;
            else if (a == "--all")       { fanTable = true; wipe = true; }
            else if (a == "--help" || a == "-h") { PrintHelp(); return; }
            else { Console.WriteLine("Unknown arg: " + a); PrintHelp(); return; }
        }

        Console.WriteLine("clear_factory_state — resets EC + UEFI to pre-MechControl state");
        if (readOnly) Console.WriteLine("MODE: --check (read-only)");
        if (!readOnly)
        {
            Console.WriteLine();
            Console.WriteLine("WARNING: clearing 0x741 / 0x7C5 / 0x7C6 drops the EC into failsafe.");
            if (fanTable)
                Console.WriteLine("         --fan-table will also wipe 0xF00-0xF5F to 0xFF. EC will");
            Console.WriteLine("         lose fan curve data. Reboot or launch MechControl soon.");
            Console.WriteLine();
        }

        int exit = 0;
        if (!uefiOnly) exit |= ClearEc(readOnly);
        if (!uefiOnly && fanTable) exit |= ClearFanTable(readOnly);
        if (!ecOnly)   exit |= ClearUefi(readOnly);
        if (wipe && !readOnly) WipeAppState();

        Console.WriteLine();
        Console.WriteLine(exit == 0 ? "Done." : "Done with errors.");
        Environment.Exit(exit);
    }

    static void PrintHelp()
    {
        Console.WriteLine("Usage: clear_factory_state.exe [options]");
        Console.WriteLine("  (no args)     Clear EC init regs + UEFI flags");
        Console.WriteLine("  --check       Read-only: show what would change");
        Console.WriteLine("  --ec-only     Clear EC registers only");
        Console.WriteLine("  --uefi-only   Clear UEFI flags only");
        Console.WriteLine("  --fan-table   Also fill 0xF00-0xF5F with 0xFF (simulates cold-boot fan table)");
        Console.WriteLine("  --wipe        Also delete userprofile_*.json, settings.json, mechcontrol.log");
        Console.WriteLine("  --all         Shortcut for --fan-table + --wipe (full nuke)");
    }
}
