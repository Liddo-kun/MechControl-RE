using System;
using System.Runtime.InteropServices;
using System.Text;

class NvApiProbe
{
    [DllImport("nvapi64.dll", EntryPoint = "nvapi_QueryInterface")] static extern IntPtr NvAPI_QueryInterface(uint id);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int InitializeDelegate();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int EnumPhysicalGPUsDelegate(IntPtr handles, ref int count);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int GetFullNameDelegate(IntPtr gpu, IntPtr name);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int GetThermalSettingsDelegate(IntPtr gpu, IntPtr settings);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int GetErrorMessageDelegate(int status, IntPtr msg);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int GenericGpuDelegate(IntPtr gpu, IntPtr buf);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    delegate int GenericDelegate(IntPtr buf);

    const uint ID_INITIALIZE           = 0x0150E828;
    const uint ID_ENUM_PHYSICAL_GPUS   = 0xE5AC921F;
    const uint ID_ENUM_LOGICAL_GPUS    = 0x48B3EA59;
    const uint ID_GPU_GET_FULL_NAME    = 0xCEEE8E9F;
    const uint ID_GPU_THERMAL_SETTINGS = 0xE3640A56;
    const uint ID_GET_ERROR_MESSAGE    = 0x6C2D048C;
    const uint ID_SYS_DRIVER_VERSION   = 0x2926AAAD;
    const uint ID_GPU_GET_BUS_ID       = 0x1BE0B8E3;
    const uint ID_GPU_GET_TACH         = 0x5F608315;
    const uint ID_GPU_CURRENT_PSTATE   = 0x927DA4F6;
    const uint ID_GPU_PSTATES20        = 0x6FF81213;

    static int TryGpuCall(uint id, IntPtr gpu, int size, int ver, string label)
    {
        IntPtr fp = NvAPI_QueryInterface(id);
        if (fp == IntPtr.Zero) { Console.WriteLine(label + ": QueryInterface NULL"); return -9999; }
        var d = (GenericGpuDelegate)Marshal.GetDelegateForFunctionPointer(fp, typeof(GenericGpuDelegate));
        IntPtr buf = Marshal.AllocHGlobal(size + 64);
        for (int i = 0; i < size + 64; i++) Marshal.WriteByte(buf, i, 0);
        Marshal.WriteInt32(buf, 0, (int)((uint)size | ((uint)ver << 16)));
        int st = d(gpu, buf);
        string extra = "";
        if (st == 0 && size >= 8) extra = " raw=[0x" + Marshal.ReadInt32(buf, 4).ToString("X8") + ",0x" + Marshal.ReadInt32(buf, 8).ToString("X8") + "]";
        Console.WriteLine(label + " id=0x" + id.ToString("X8") + " size=" + size + " ver=" + ver + " -> " + Err(st) + extra);
        Marshal.FreeHGlobal(buf);
        return st;
    }

    static GetErrorMessageDelegate _errMsg;

    static string Err(int status)
    {
        if (_errMsg == null) return status.ToString();
        IntPtr buf = Marshal.AllocHGlobal(64);
        try
        {
            if (_errMsg(status, buf) != 0) return status.ToString();
            return status + " (" + Marshal.PtrToStringAnsi(buf) + ")";
        }
        finally { Marshal.FreeHGlobal(buf); }
    }

    static int ScanMode(string candidatesFile)
    {
        int valid = 0;
        foreach (string line in System.IO.File.ReadAllLines(candidatesFile))
        {
            string t = line.Trim();
            if (t.Length == 0) continue;
            uint id = Convert.ToUInt32(t, 16);
            if (NvAPI_QueryInterface(id) != IntPtr.Zero)
            {
                Console.WriteLine("VALID 0x" + id.ToString("X8"));
                valid++;
            }
        }
        Console.WriteLine(valid + " valid function pointers found.");
        return 0;
    }

    static T GetDelegate<T>(uint id) where T : class
    {
        IntPtr p = NvAPI_QueryInterface(id);
        if (p == IntPtr.Zero) return null;
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }

    static int Main(string[] args)
    {
        var init = GetDelegate<InitializeDelegate>(ID_INITIALIZE);
        if (init == null) { Console.WriteLine("No NvAPI_Initialize"); return 1; }
        int st = init();
        if (st != 0) { Console.WriteLine("NvAPI_Initialize failed: " + st); return 1; }

        if (args.Length >= 2 && args[0] == "scan")
            return ScanMode(args[1]);

        _errMsg = GetDelegate<GetErrorMessageDelegate>(ID_GET_ERROR_MESSAGE);

        var enumGpus = GetDelegate<EnumPhysicalGPUsDelegate>(ID_ENUM_PHYSICAL_GPUS);
        var getName  = GetDelegate<GetFullNameDelegate>(ID_GPU_GET_FULL_NAME);
        var getThermal = GetDelegate<GetThermalSettingsDelegate>(ID_GPU_THERMAL_SETTINGS);
        Console.WriteLine("EnumPhysicalGPUs ptr: " + (enumGpus != null ? "OK" : "NULL"));
        Console.WriteLine("GetThermalSettings ptr: " + (getThermal != null ? "OK" : "NULL"));
        if (enumGpus == null || getThermal == null) return 1;

        IntPtr handles = Marshal.AllocHGlobal(64 * IntPtr.Size);
        int count = 64;
        st = enumGpus(handles, ref count);
        Console.WriteLine("EnumPhysicalGPUs: " + Err(st) + ", count=" + count);
        if (st != 0 || count == 0) return 1;

        IntPtr gpu = Marshal.ReadIntPtr(handles, 0);
        if (getName != null)
        {
            IntPtr name = Marshal.AllocHGlobal(64);
            if (getName(gpu, name) == 0)
                Console.WriteLine("GPU[0]: " + Marshal.PtrToStringAnsi(name));
            Marshal.FreeHGlobal(name);
        }

        // Probe GetThermalSettings with varying struct versions and sensor-array sizes.
        // Sensor entry = 5 x u32 (controller, defaultMin, defaultMax, current, target) = 20 bytes.
        for (int ver = 1; ver <= 5; ver++)
        {
            var line = new StringBuilder("ver=" + ver + ": ");
            for (int n = 1; n <= 16; n++)
            {
                int size = 8 + n * 20;
                IntPtr buf = Marshal.AllocHGlobal(size + 64);
                for (int i = 0; i < size + 64; i++) Marshal.WriteByte(buf, i, 0xCC);
                Marshal.WriteInt32(buf, 0, (int)((uint)size | ((uint)ver << 16)));
                Marshal.WriteInt32(buf, 4, n);
                st = getThermal(gpu, buf);
                if (st == 0)
                {
                    int retCount = Marshal.ReadInt32(buf, 4);
                    line.Append("\n  OK reqN=" + n + " size=" + size + " count=" + retCount);
                    for (int i = 0; i < Math.Min(retCount, n) && i < 32; i++)
                    {
                        int off = 8 + i * 20;
                        int controller = Marshal.ReadInt32(buf, off);
                        int minT = Marshal.ReadInt32(buf, off + 4);
                        int maxT = Marshal.ReadInt32(buf, off + 8);
                        int curT = Marshal.ReadInt32(buf, off + 12);
                        int target = Marshal.ReadInt32(buf, off + 16);
                        line.Append("\n    [" + i + "] controller=" + controller + " target=" + target
                            + " cur=" + curT + " min=" + minT + " max=" + maxT);
                    }
                }
                else
                {
                    line.Append(" n" + n + "=" + st);
                }
                Marshal.FreeHGlobal(buf);
            }
            Console.WriteLine(line.ToString());
        }

        Console.WriteLine("--- function battery (physical handle) ---");
        TryGpuCall(ID_GPU_GET_BUS_ID, gpu, 8, 1, "GetBusId");
        TryGpuCall(ID_GPU_GET_TACH, gpu, 8, 1, "GetTachReading");
        TryGpuCall(ID_GPU_CURRENT_PSTATE, gpu, 12, 2, "GetCurrentPstate");
        TryGpuCall(ID_GPU_PSTATES20, gpu, 8 + 8 + 16 * 32, 2, "GetPstates20");
        TryGpuCall(ID_GPU_THERMAL_SETTINGS, gpu, 68, 2, "GetThermalSettings");

        Console.WriteLine("--- logical handle retry ---");
        IntPtr lp = NvAPI_QueryInterface(ID_ENUM_LOGICAL_GPUS);
        if (lp != IntPtr.Zero)
        {
            var enumLogical = (EnumPhysicalGPUsDelegate)Marshal.GetDelegateForFunctionPointer(lp, typeof(EnumPhysicalGPUsDelegate));
            IntPtr lh = Marshal.AllocHGlobal(64 * IntPtr.Size);
            int lcount = 64;
            int lst = enumLogical(lh, ref lcount);
            Console.WriteLine("EnumLogicalGPUs: " + Err(lst) + " count=" + lcount);
            if (lst == 0 && lcount > 0)
            {
                IntPtr lgpu = Marshal.ReadIntPtr(lh, 0);
                TryGpuCall(ID_GPU_THERMAL_SETTINGS, lgpu, 68, 2, "GetThermalSettings(logical)");
            }
            Marshal.FreeHGlobal(lh);
        }

        Console.WriteLine("Done.");
        return 0;
    }
}
