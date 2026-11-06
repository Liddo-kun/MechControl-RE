// Reads or sets the NVIDIA driver setting "CUDA - Force P2 State" (0x50166C5E) in the base
// (global) driver profile through NVAPI DRS. Usage: DrsForceP2 get | on | off | default
// Compile: csc.exe -nologo DrsForceP2.cs  (setting it needs an elevated shell)
using System;
using System.Runtime.InteropServices;

class DrsForceP2
{
    static uint SettingId = 0x50166C5E;
    const int SettingSize = 12320; // NVDRS_SETTING_V1, pack(4)
    const int SettingNotFound = -160;

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr QueryInterface(uint id);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int NoArgs();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int OutHandle(out IntPtr handle);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Session(IntPtr session);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetProfile(IntPtr session, out IntPtr profile);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetSetting(IntPtr session, IntPtr profile, uint id, byte[] setting);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SetSetting(IntPtr session, IntPtr profile, byte[] setting);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int FindApp(IntPtr session, ushort[] name, out IntPtr profile, byte[] app);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int ById(IntPtr session, IntPtr profile, uint id);

    [DllImport("kernel32")] static extern IntPtr LoadLibrary(string path);
    [DllImport("kernel32")] static extern IntPtr GetProcAddress(IntPtr module, string name);

    static QueryInterface query;
    static T F<T>(uint id) where T : class
    {
        IntPtr p = query(id);
        if (p == IntPtr.Zero) throw new Exception(string.Format("NVAPI 0x{0:X8} unavailable", id));
        return Marshal.GetDelegateForFunctionPointer(p, typeof(T)) as T;
    }
    static void Check(int status, string name) { if (status != 0) throw new Exception(name + " failed: " + status); }

    static int Main(string[] args)
    {
        string mode = args.Length > 0 ? args[0] : "get";
        uint value = 0; if (mode == "set") { SettingId = Convert.ToUInt32(args[1], 16); value = Convert.ToUInt32(args[2]); } if (mode == "read" || mode == "reset") SettingId = Convert.ToUInt32(args[1], 16);
        IntPtr lib = LoadLibrary("nvapi64.dll");
        query = (QueryInterface)Marshal.GetDelegateForFunctionPointer(GetProcAddress(lib, "nvapi_QueryInterface"), typeof(QueryInterface));
        Check(F<NoArgs>(0x0150E828)(), "Initialize");
        IntPtr session, profile;
        Check(F<OutHandle>(0x0694D52E)(out session), "DRS_CreateSession");
        try
        {
            Check(F<Session>(0x375DBD6B)(session), "DRS_LoadSettings");
            Check(F<GetProfile>(0xDA8466A0)(session, out profile), "DRS_GetBaseProfile");
            if (mode == "global")
                Check(F<GetProfile>(0x617BFF9F)(session, out profile), "DRS_GetCurrentGlobalProfile");
            if (mode == "app")
            {
                // NVDRS_APPLICATION_V4: version, isPredefined, 4 names, flags, commandLine = 20488 bytes.
                int[] sizes = { 0, 12296, 12296, 16392, 20488 }; int ver = int.Parse(args[2]); var app = new byte[sizes[ver]];
                BitConverter.GetBytes(sizes[ver] | (ver << 16)).CopyTo(app, 0);
                var name = new ushort[2048]; for (int i = 0; i < args[1].Length; i++) name[i] = args[1][i];
                int r = F<FindApp>(0xEEE566B2)(session, name, out profile, app);
                if (r != 0) { Console.WriteLine("no application profile for " + args[1] + " (status " + r + ")"); return 0; }
                Console.WriteLine("application profile found (predefined {0})", BitConverter.ToUInt32(app, 4));
            }
            if (mode == "set") mode = value == 1 ? "on" : value == 0 ? "off" : "raw";
            if (mode == "reset") mode = "default";
            if (mode == "on" || mode == "off" || mode == "raw")
            {
                var s = new byte[SettingSize];
                BitConverter.GetBytes(SettingSize | (1 << 16)).CopyTo(s, 0);
                BitConverter.GetBytes(SettingId).CopyTo(s, 4100);
                // settingType 0 = DWORD; u32CurrentValue at 8220.
                BitConverter.GetBytes(mode == "raw" ? value : mode == "on" ? 1u : 0u).CopyTo(s, 8220);
                Check(F<SetSetting>(0x577DD202)(session, profile, s), "DRS_SetSetting");
                Check(F<Session>(0xFCBC7E14)(session), "DRS_SaveSettings");
            }
            else if (mode == "default")
            {
                int r = F<ById>(0xE4A26362)(session, profile, SettingId);
                if (r != 0 && r != SettingNotFound) Check(r, "DRS_DeleteProfileSetting");
                Check(F<Session>(0xFCBC7E14)(session), "DRS_SaveSettings");
            }
            var g = new byte[SettingSize];
            BitConverter.GetBytes(SettingSize | (1 << 16)).CopyTo(g, 0);
            int status = F<GetSetting>(0x73BF8338)(session, profile, SettingId, g);
            if (status == SettingNotFound) { Console.WriteLine("not set in base profile (driver default: On)"); return 0; }
            Check(status, "DRS_GetSetting");
            Console.WriteLine("value {0} ({1}); location {2}; predefined valid {3} value {4}",
                BitConverter.ToUInt32(g, 8220), BitConverter.ToUInt32(g, 8220) == 0 ? "Off" : "On",
                BitConverter.ToUInt32(g, 4108), BitConverter.ToUInt32(g, 4116), BitConverter.ToUInt32(g, 4120));
            return 0;
        }
        finally { F<Session>(0xDAD9CFF8)(session); }
    }
}
