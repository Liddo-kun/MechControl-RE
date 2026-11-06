using System;
using System.Reflection;
using System.Linq;
using System.IO;
using System.Collections.Generic;

class Program
{
    static Type[] allTypes;

    static void Main()
    {
        AppDomain.CurrentDomain.ReflectionOnlyAssemblyResolve += delegate(object sender, ResolveEventArgs args)
        {
            try { return Assembly.ReflectionOnlyLoad(args.Name); }
            catch { return null; }
        };

        string oemBase = null;
        foreach (string d in Directory.GetDirectories(@"C:\Program Files\OEM"))
        {
            if (Directory.Exists(Path.Combine(d, "AiStoneService")))
            { oemBase = d; break; }
        }
        string gcuPath = Path.Combine(oemBase, @"AiStoneService\MyControlCenter\GCUService.exe");
        Assembly asm = Assembly.ReflectionOnlyLoadFrom(gcuPath);

        try { allTypes = asm.GetTypes(); }
        catch (ReflectionTypeLoadException ex)
        {
            allTypes = ex.Types.Where(t => t != null).ToArray();
        }

        StreamWriter w = new StreamWriter(@"C:\Users\Jon\clean\details_dump.txt", false);

        // 1. Find MyFanManager class and its methods — this tells us how fan tables work
        w.WriteLine("=== MyFanManager methods ===");
        DumpTypeMethods(w, "MyFanManager_Intel");
        DumpTypeMethods(w, "MyFanManager");

        // 2. Find battery-related types
        w.WriteLine("\n=== Battery types and methods ===");
        foreach (var t in allTypes)
        {
            try
            {
                if (t.Name.Contains("Battery") || t.Name.Contains("BatPro"))
                {
                    w.WriteLine("\n--- " + t.FullName + " ---");
                    var methods = t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                    foreach (var m in methods)
                    {
                        try { w.WriteLine("  " + m.ReturnType.Name + " " + m.Name + "(...)"); }
                        catch { }
                    }
                    var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                    foreach (var f in fields)
                    {
                        try
                        {
                            string val = f.IsLiteral ? f.GetRawConstantValue().ToString() : "(runtime)";
                            w.WriteLine("  FIELD: " + f.FieldType.Name + " " + f.Name + " = " + val);
                        }
                        catch { }
                    }
                }
            }
            catch { }
        }

        // 3. Find display/local dimming handler
        w.WriteLine("\n=== Setting/Display handler types ===");
        foreach (var t in allTypes)
        {
            try
            {
                if (t.Name.Contains("Setting") && !t.Name.Contains("<") && !t.Name.Contains("AppSettings"))
                {
                    w.WriteLine("  " + t.FullName);
                }
            }
            catch { }
        }

        // 4. Look at OemDisplayMode in GCUBridge context
        w.WriteLine("\n=== Types with 'DisplayMode' or 'Dimm' ===");
        foreach (var t in allTypes)
        {
            try
            {
                if ((t.Name.Contains("DisplayMode") || t.Name.Contains("Dimm") || t.Name.Contains("MiniLed")) && !t.Name.Contains("<"))
                {
                    w.WriteLine("\n--- " + t.FullName + " ---");
                    var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                    foreach (var f in fields)
                    {
                        try
                        {
                            string val = f.IsLiteral ? f.GetRawConstantValue().ToString() : "(runtime)";
                            w.WriteLine("  " + f.FieldType.Name + " " + f.Name + " = " + val);
                        }
                        catch { }
                    }
                }
            }
            catch { }
        }

        // 5. Look for how the fan table version is determined per project
        w.WriteLine("\n=== Types with 'RamFan' in name ===");
        foreach (var t in allTypes)
        {
            try
            {
                if (t.Name.Contains("RamFan") && !t.Name.Contains("<"))
                {
                    w.WriteLine("  " + t.FullName);
                }
            }
            catch { }
        }

        // 6. Look for AcpiCtrl to understand driver interface
        w.WriteLine("\n=== MyECIO types ===");
        foreach (var t in allTypes)
        {
            try
            {
                if (t.Namespace != null && t.Namespace.Contains("MyECIO"))
                {
                    w.WriteLine("\n--- " + t.FullName + " ---");
                    var methods = t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                    foreach (var m in methods)
                    {
                        try
                        {
                            string parms = "";
                            foreach (var p in m.GetParameters())
                                parms += p.ParameterType.Name + " " + p.Name + ", ";
                            w.WriteLine("  " + m.ReturnType.Name + " " + m.Name + "(" + parms.TrimEnd(',', ' ') + ")");
                        }
                        catch { }
                    }
                    var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                    foreach (var f in fields)
                    {
                        try
                        {
                            string val = "?";
                            if (f.IsLiteral)
                            {
                                object raw = f.GetRawConstantValue();
                                if (raw is int) val = "0x" + ((int)raw).ToString("X");
                                else if (raw is uint) val = "0x" + ((uint)raw).ToString("X");
                                else val = raw.ToString();
                            }
                            else
                            {
                                val = "(" + (f.IsStatic ? "static" : "instance") + ")";
                            }
                            w.WriteLine("  FIELD: " + f.FieldType.Name + " " + f.Name + " = " + val);
                        }
                        catch { }
                    }
                }
            }
            catch { }
        }

        // 7. Search for IOCTL constants
        w.WriteLine("\n=== Looking for IOCTL fields ===");
        foreach (var t in allTypes)
        {
            try
            {
                var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                foreach (var f in fields)
                {
                    try
                    {
                        if (f.Name.Contains("IOCTL") && f.IsLiteral)
                        {
                            object raw = f.GetRawConstantValue();
                            string val;
                            if (raw is int) val = "0x" + ((int)raw).ToString("X");
                            else if (raw is uint) val = "0x" + ((uint)raw).ToString("X");
                            else val = raw.ToString();
                            w.WriteLine("  " + t.FullName + "." + f.Name + " = " + val);
                        }
                    }
                    catch { }
                }
            }
            catch { }
        }

        // 8. Look for device path
        w.WriteLine("\n=== Looking for device path fields ===");
        foreach (var t in allTypes)
        {
            try
            {
                var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                foreach (var f in fields)
                {
                    try
                    {
                        if (f.IsLiteral && f.FieldType.Name == "String")
                        {
                            string val = (string)f.GetRawConstantValue();
                            if (val != null && (val.Contains(@"\\.\") || val.Contains("Device") || val.Contains("driver")))
                                w.WriteLine("  " + t.FullName + "." + f.Name + " = \"" + val + "\"");
                        }
                    }
                    catch { }
                }
            }
            catch { }
        }

        w.Close();
        Console.WriteLine("Details written to details_dump.txt");
    }

    static void DumpTypeMethods(StreamWriter w, string typeName)
    {
        foreach (var t in allTypes)
        {
            try
            {
                if (t.Name == typeName)
                {
                    w.WriteLine("\n--- " + t.FullName + " ---");
                    var methods = t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                    foreach (var m in methods)
                    {
                        try
                        {
                            string parms = "";
                            foreach (var p in m.GetParameters())
                                parms += p.ParameterType.Name + " " + p.Name + ", ";
                            w.WriteLine("  " + m.ReturnType.Name + " " + m.Name + "(" + parms.TrimEnd(',', ' ') + ")");
                        }
                        catch { }
                    }
                }
            }
            catch { }
        }
    }
}
