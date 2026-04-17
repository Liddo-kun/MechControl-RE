using System.Runtime.InteropServices;

namespace MechControl.Features;

/// <summary>
/// One-time UEFI NVRAM setup for standalone operation.
/// Sets OemMagicVariable flags (ApExistFlag, ApUseFlag, PowerMode, MemoryOverClockSwitch)
/// so the BIOS initializes the EC with valid fan tables and power mode at boot.
///
/// Not strictly required — MechControl's EC init handles everything at runtime —
/// but ensures the EC is in a sane state during the boot-to-app-launch window.
///
/// Only runs once; skips if flags are already set.
/// </summary>
static class UefiSetup
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern uint GetFirmwareEnvironmentVariableW(
        string lpName, string lpGuid, IntPtr pBuffer, uint nSize);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool SetFirmwareEnvironmentVariableW(
        string lpName, string lpGuid, IntPtr pValue, uint nSize);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool LookupPrivilegeValue(string? sys, string name, out LUID luid);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool AdjustTokenPrivileges(IntPtr tok, bool disableAll,
        ref TOKEN_PRIVILEGES tp, uint len, IntPtr prev, IntPtr retLen);

    [DllImport("kernel32.dll")]
    private static extern IntPtr GetCurrentProcess();

    [DllImport("kernel32.dll")]
    private static extern bool CloseHandle(IntPtr handle);

    [StructLayout(LayoutKind.Sequential)]
    private struct LUID { public uint LowPart; public int HighPart; }

    [StructLayout(LayoutKind.Sequential)]
    private struct LUID_AND_ATTRIBUTES { public LUID Luid; public uint Attributes; }

    [StructLayout(LayoutKind.Sequential)]
    private struct TOKEN_PRIVILEGES { public uint PrivilegeCount; public LUID_AND_ATTRIBUTES Privileges; }

    private const string OemGuid = "{9f33f85c-13ca-4fd1-9c4a-96217722c593}";
    private const string VarName = "OemMagicVariable";

    // NVRAM_STRUCT offsets
    private const int OffPowerMode = 0x2F;
    private const int OffMemOcSwitch = 0x33;
    private const int OffApExist = 0x5D;
    private const int OffApUse = 0x61;

    private static bool EnablePrivilege()
    {
        if (!OpenProcessToken(GetCurrentProcess(), 0x0028, out IntPtr tok))
            return false;
        try
        {
            if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out LUID luid))
                return false;
            var tp = new TOKEN_PRIVILEGES();
            tp.PrivilegeCount = 1;
            tp.Privileges.Luid = luid;
            tp.Privileges.Attributes = 2;
            AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
            return Marshal.GetLastWin32Error() == 0;
        }
        finally
        {
            CloseHandle(tok);
        }
    }

    /// <summary>
    /// Check and fix UEFI NVRAM flags if needed. No-op if already correct.
    /// </summary>
    public static void EnsureFlags()
    {
        try
        {
            if (!EnablePrivilege())
            {
                DiagnosticLog.Warn("UEFI: could not enable SeSystemEnvironmentPrivilege");
                return;
            }

            IntPtr buf = Marshal.AllocHGlobal(4096);
            uint size = GetFirmwareEnvironmentVariableW(VarName, OemGuid, buf, 4096);
            if (size == 0)
            {
                Marshal.FreeHGlobal(buf);
                DiagnosticLog.Write("UEFI: OemMagicVariable not found (non-Uniwill BIOS?) — skipping");
                return;
            }

            byte[] data = new byte[size];
            Marshal.Copy(buf, data, 0, (int)size);
            Marshal.FreeHGlobal(buf);

            // Structural validation — if any of these fail, the NVRAM_STRUCT
            // layout doesn't match our assumptions and we must not write
            if (size != 180)
            {
                DiagnosticLog.Warn($"UEFI: OemMagicVariable unexpected size ({size} bytes, expected 180) — skipping");
                return;
            }
            if (data[OffPowerMode] > 3)
            {
                DiagnosticLog.Warn($"UEFI: PowerMode=0x{data[OffPowerMode]:X2} out of range (expected 0-3) — struct layout may differ, skipping");
                return;
            }
            if (data[OffApExist] > 1 || data[OffApUse] > 1 || data[OffMemOcSwitch] > 1)
            {
                DiagnosticLog.Warn($"UEFI: boolean flags out of range (ApExist={data[OffApExist]}, ApUse={data[OffApUse]}, MemOC={data[OffMemOcSwitch]}) — struct layout may differ, skipping");
                return;
            }

            // Check if all flags are already correct
            if (data[OffPowerMode] == 3 &&
                data[OffMemOcSwitch] == 1 &&
                data[OffApExist] == 1 &&
                data[OffApUse] == 1)
            {
                DiagnosticLog.Write("UEFI: flags already set correctly");
                return;
            }

            // Apply fixes
            DiagnosticLog.Write($"UEFI: fixing flags (PowerMode={data[OffPowerMode]}, " +
                $"MemOC={data[OffMemOcSwitch]}, ApExist={data[OffApExist]}, ApUse={data[OffApUse]})");

            data[OffPowerMode] = 3;
            data[OffMemOcSwitch] = 1;
            data[OffApExist] = 1;
            data[OffApUse] = 1;

            IntPtr wBuf = Marshal.AllocHGlobal((int)size);
            Marshal.Copy(data, 0, wBuf, (int)size);
            bool ok = SetFirmwareEnvironmentVariableW(VarName, OemGuid, wBuf, size);
            Marshal.FreeHGlobal(wBuf);

            if (!ok)
            {
                DiagnosticLog.Warn($"UEFI: write failed (error {Marshal.GetLastWin32Error()})");
                return;
            }

            // Verify readback
            IntPtr vBuf = Marshal.AllocHGlobal(4096);
            uint vSize = GetFirmwareEnvironmentVariableW(VarName, OemGuid, vBuf, 4096);
            if (vSize > 0)
            {
                byte[] verify = new byte[vSize];
                Marshal.Copy(vBuf, verify, 0, (int)vSize);
                bool match = verify[OffPowerMode] == 3 && verify[OffMemOcSwitch] == 1
                    && verify[OffApExist] == 1 && verify[OffApUse] == 1;
                DiagnosticLog.Write(match
                    ? "UEFI: flags updated and verified (takes effect next reboot)"
                    : "UEFI: write succeeded but readback mismatch");
            }
            else
            {
                DiagnosticLog.Write("UEFI: flags written (readback skipped)");
            }
            Marshal.FreeHGlobal(vBuf);
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"UEFI setup failed: {ex.Message}");
        }
    }
}
