using System;
using System.Runtime.InteropServices;
using System.Text;

class CpuzGpuRegs
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, byte[] inBuf, uint inLen, byte[] outBuf, uint outLen, out uint ret, IntPtr ov);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    const uint GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2;
    const uint OPEN_EXISTING = 3;

    const uint IOCTL_READ_PHYS = 0x9C402540;   // in {u32 ?, u32 addr, u32 size} -> out {u32 size, u32 value}
    const uint IOCTL_READ_PCI_BAR_DWORD = 0x9C4025D4;
    const ulong GPU_BAR0 = 0x80000000;
    const uint NVIDIA_BDF = 0x80020000;
    static readonly uint[] BlackwellThermalOffsets = {
        0x00AD0A90, 0x00AD0A94, 0x00AD0A98,
        0x00AD0A9C, 0x00AD0AA0, 0x00AD0AA4
    };

    [DllImport("ntdll.dll")] static extern int NtOpenDirectoryObject(out IntPtr h, uint access, ref OBJECT_ATTRIBUTES oa);
    [DllImport("ntdll.dll")] static extern int NtQueryDirectoryObject(IntPtr h, IntPtr buf, int len, bool single, bool restart, ref uint ctx, out uint retLen);
    [DllImport("ntdll.dll")] static extern int NtClose(IntPtr h);

    [StructLayout(LayoutKind.Sequential)]
    struct UNICODE_STRING { public ushort Length, MaximumLength; public IntPtr Buffer; }
    [StructLayout(LayoutKind.Sequential)]
    struct OBJECT_ATTRIBUTES { public int Length; public IntPtr RootDir; public IntPtr Name; public int Attr; public IntPtr SD, SQOS; }

    static void EnumObjDir(string path, string filter)
    {
        IntPtr nameBuf = Marshal.StringToHGlobalUni(path);
        var us = new UNICODE_STRING { Length = (ushort)(path.Length * 2), MaximumLength = (ushort)(path.Length * 2 + 2), Buffer = nameBuf };
        IntPtr usBuf = Marshal.AllocHGlobal(Marshal.SizeOf(us));
        Marshal.StructureToPtr(us, usBuf, false);
        var oa = new OBJECT_ATTRIBUTES { Length = Marshal.SizeOf(typeof(OBJECT_ATTRIBUTES)), Name = usBuf, Attr = 0x40 }; // OBJ_CASE_INSENSITIVE
        IntPtr h; int st = NtOpenDirectoryObject(out h, 1, ref oa);
        if (st != 0) { Console.WriteLine(path + " open err 0x" + st.ToString("X8")); return; }
        IntPtr buf = Marshal.AllocHGlobal(1024);
        uint ctx = 0; bool restart = true;
        while (true)
        {
            uint retLen; st = NtQueryDirectoryObject(h, buf, 1024, true, restart, ref ctx, out retLen);
            restart = false;
            if (st != 0) break;
            ushort nlen = (ushort)Marshal.ReadInt16(buf, 0);
            IntPtr nbuf = Marshal.ReadIntPtr(buf, 8);
            string name = Marshal.PtrToStringUni(nbuf, nlen / 2);
            ushort tlen = (ushort)Marshal.ReadInt16(buf, 16);
            IntPtr tbuf = Marshal.ReadIntPtr(buf, 24);
            string type = tbuf != IntPtr.Zero ? Marshal.PtrToStringUni(tbuf, tlen / 2) : "";
            if (filter == null || name.ToLowerInvariant().Contains(filter))
                Console.WriteLine("  " + type.PadRight(14) + " " + name);
        }
        Marshal.FreeHGlobal(buf); NtClose(h);
    }

    [DllImport("ntdll.dll")] static extern int NtCreateFile(out IntPtr h, uint access, ref OBJECT_ATTRIBUTES oa, IntPtr iosb, IntPtr allocSize, uint fileAttrs, uint share, uint disp, uint options, IntPtr ea, uint eaLen);

    static IntPtr OpenDevNative(string path)
    {
        IntPtr nameBuf = Marshal.StringToHGlobalUni(path);
        var us = new UNICODE_STRING { Length = (ushort)(path.Length * 2), MaximumLength = (ushort)(path.Length * 2 + 2), Buffer = nameBuf };
        IntPtr usBuf = Marshal.AllocHGlobal(Marshal.SizeOf(us));
        Marshal.StructureToPtr(us, usBuf, false);
        var oa = new OBJECT_ATTRIBUTES { Length = Marshal.SizeOf(typeof(OBJECT_ATTRIBUTES)), Name = usBuf, Attr = 0x40 };
        IntPtr iosb = Marshal.AllocHGlobal(16);
        IntPtr h;
        int st = NtCreateFile(out h, 0xC0000000, ref oa, iosb, IntPtr.Zero, 0, 3, 1, 0, IntPtr.Zero, 0);
        Console.WriteLine(path + " NtCreateFile -> 0x" + st.ToString("X8"));
        Marshal.FreeHGlobal(iosb); Marshal.FreeHGlobal(usBuf); Marshal.FreeHGlobal(nameBuf);
        return st == 0 ? h : new IntPtr(-1);
    }

    static IntPtr OpenDev()
    {
        string[] names = { @"\\.\cpuz165", @"\\.\Global\cpuz165", @"\\.\GLOBALROOT\Device\cpuz165" };
        foreach (var n in names)
        {
            IntPtr h = CreateFileW(n, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
            if (h != new IntPtr(-1)) { Console.WriteLine("opened " + n); return h; }
            Console.WriteLine(n + " -> err " + Marshal.GetLastWin32Error());
        }
        return OpenDevNative(@"\Device\cpuz165");
    }

    static uint ReadPhysDword(IntPtr h, ulong addr)
    {
        byte[] inB = new byte[12];
        BitConverter.GetBytes(0u).CopyTo(inB, 0);
        BitConverter.GetBytes((uint)addr).CopyTo(inB, 4); // NOTE: 32-bit addr observed; BAR0 fits
        BitConverter.GetBytes(4u).CopyTo(inB, 8);
        byte[] outB = new byte[8];
        uint ret;
        bool ok = DeviceIoControl(h, IOCTL_READ_PHYS, inB, 12, outB, 8, out ret, IntPtr.Zero);
        if (!ok) { Console.WriteLine("  IOCTL_READ_PHYS err " + Marshal.GetLastWin32Error()); return 0xFFFFFFFF; }
        return BitConverter.ToUInt32(outB, 4);
    }

    static bool ReadPciBarDword(IntPtr h, uint encodedBdf, uint offset, out uint value)
    {
        byte[] inB = new byte[16];
        BitConverter.GetBytes(encodedBdf).CopyTo(inB, 0);
        BitConverter.GetBytes(0x10u).CopyTo(inB, 4); // PCI BAR0 configuration register
        BitConverter.GetBytes(1u).CopyTo(inB, 8);    // permit a 64-bit BAR
        BitConverter.GetBytes(offset).CopyTo(inB, 12);
        byte[] outB = new byte[8];
        uint ret;
        bool ok = DeviceIoControl(h, IOCTL_READ_PCI_BAR_DWORD, inB, 16, outB, 8, out ret, IntPtr.Zero);
        value = ok && ret >= 8 ? BitConverter.ToUInt32(outB, 4) : 0;
        return ok && ret >= 8 && BitConverter.ToUInt32(outB, 0) == 1;
    }

    static void ReadTemperatures(IntPtr h)
    {
        foreach (uint offset in BlackwellThermalOffsets)
        {
            uint raw;
            bool ok = ReadPciBarDword(h, NVIDIA_BDF, offset, out raw);
            bool allOnes = raw == 0xFFFFFFFF;
            bool valid = ok && !allOnes && (raw & 0x40000000) != 0;
            Console.WriteLine("+0x" + offset.ToString("X8") + " ok=" + ok + " raw=0x" + raw.ToString("X8")
                + (allOnes ? " unavailable (all ones)" :
                    valid ? " temp=" + ((raw & 0xffff) / 256.0).ToString("F3") + " C" : " invalid"));
        }
    }

    static void Dump(IntPtr h, uint offset, int dwordCount)
    {
        for (int i = 0; i < dwordCount; i++)
        {
            uint v = ReadPhysDword(h, GPU_BAR0 + offset + (uint)(i * 4));
            if (v != 0 && v != 0xFFFFFFFF)
                Console.WriteLine("  +0x" + (offset + i * 4).ToString("X6") + " = 0x" + v.ToString("X8") + " (" + v + ")");
        }
    }

    static int Main(string[] args)
    {
        if (args.Length == 0) { Console.WriteLine("usage: temps | polltemps <sec> | boot0 | dump <hexoff> <dwords> | poll <hexoff> <dwords> <sec> | objdir <path> [filter]"); return 1; }
        if (args[0] == "objdir") { EnumObjDir(args[1], args.Length > 2 ? args[2].ToLowerInvariant() : null); return 0; }
        IntPtr h = OpenDev();
        if (h == new IntPtr(-1)) return 1;
        string mode = args[0];

        if (mode == "temps")
        {
            ReadTemperatures(h);
        }
        else if (mode == "polltemps" && args.Length >= 2)
        {
            DateTime end = DateTime.Now.AddSeconds(int.Parse(args[1]));
            while (DateTime.Now < end)
            {
                Console.WriteLine(DateTime.Now.ToString("HH:mm:ss.ff"));
                ReadTemperatures(h);
                System.Threading.Thread.Sleep(1000);
            }
        }
        else if (mode == "boot0")
        {
            Console.WriteLine("BOOT0 (BAR0+0) via READ_PHYS: 0x" + ReadPhysDword(h, GPU_BAR0).ToString("X8"));
        }
        else if (mode == "dump" && args.Length >= 3)
        {
            uint off = Convert.ToUInt32(args[1], 16);
            int n = int.Parse(args[2]);
            Dump(h, off, n);
        }
        else if (mode == "poll" && args.Length >= 4)
        {
            uint off = Convert.ToUInt32(args[1], 16);
            int n = int.Parse(args[2]);
            int sec = int.Parse(args[3]);
            uint[] prev = new uint[n];
            DateTime end = DateTime.Now.AddSeconds(sec);
            while (DateTime.Now < end)
            {
                var sb = new StringBuilder(DateTime.Now.ToString("HH:mm:ss.ff") + " ");
                for (int i = 0; i < n; i++)
                {
                    uint v = ReadPhysDword(h, GPU_BAR0 + off + (uint)(i * 4));
                    if (v != prev[i])
                    {
                        sb.Append("+0x" + (off + i * 4).ToString("X4") + "=" + v.ToString("X8") + "(" + v + ") ");
                        prev[i] = v;
                    }
                }
                Console.WriteLine(sb.ToString());
                System.Threading.Thread.Sleep(500);
            }
        }
        CloseHandle(h);
        return 0;
    }
}
