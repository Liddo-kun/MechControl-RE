using System;
using System.Reflection;
using System.Linq;
using System.IO;
using System.Collections.Generic;

class Program
{
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

        Type[] types;
        try { types = asm.GetTypes(); }
        catch (ReflectionTypeLoadException ex)
        {
            types = ex.Types.Where(t => t != null).ToArray();
        }

        // PART 1: Dump ALL ECSpec fields
        StreamWriter w = new StreamWriter(@"C:\Users\Jon\clean\ecspec_dump.txt", false);
        foreach (var t in types.Where(t2 => t2.Name == "ECSpec" && t2.Namespace == "Define"))
        {
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance);
            w.WriteLine("=== Define.ECSpec : " + fields.Length + " total fields ===");
            w.WriteLine();

            // Group by prefix
            foreach (var f in fields.OrderBy(f2 => f2.Name))
            {
                string val = "?";
                if (f.IsLiteral)
                {
                    try
                    {
                        object raw = f.GetRawConstantValue();
                        if (raw is int) val = "0x" + ((int)raw).ToString("X");
                        else if (raw is uint) val = "0x" + ((uint)raw).ToString("X");
                        else if (raw is ushort) val = "0x" + ((ushort)raw).ToString("X");
                        else if (raw is byte) val = "0x" + ((byte)raw).ToString("X2");
                        else val = raw != null ? raw.ToString() : "null";
                    }
                    catch { val = "error"; }
                }
                w.WriteLine(f.FieldType.Name + " " + f.Name + " = " + val);
            }
        }

        // Also dump RamFan ECSpec variants
        foreach (var t in types.Where(t2 => t2.Name.Contains("ECSpec")))
        {
            if (t.Name == "ECSpec") continue;
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance);
            w.WriteLine("\n=== " + t.FullName + " : " + fields.Length + " fields ===");
            foreach (var f in fields.OrderBy(f2 => f2.Name))
            {
                string val = "?";
                if (f.IsLiteral)
                {
                    try
                    {
                        object raw = f.GetRawConstantValue();
                        if (raw is int) val = "0x" + ((int)raw).ToString("X");
                        else if (raw is uint) val = "0x" + ((uint)raw).ToString("X");
                        else if (raw is ushort) val = "0x" + ((ushort)raw).ToString("X");
                        else if (raw is byte) val = "0x" + ((byte)raw).ToString("X2");
                        else val = raw != null ? raw.ToString() : "null";
                    }
                    catch { val = "error"; }
                }
                w.WriteLine(f.FieldType.Name + " " + f.Name + " = " + val);
            }
        }

        w.Close();
        Console.WriteLine("ECSpec dump written to ecspec_dump.txt");

        // PART 2: Search for "IDY" and "Project" in type/field names
        Console.WriteLine("\n=== Types containing 'Project' or 'IDY' ===");
        foreach (var t in types)
        {
            try
            {
                if (t.Name.Contains("Project") || t.Name.Contains("IDY"))
                    Console.WriteLine("  TYPE: " + t.FullName);
            }
            catch { }
        }

        // PART 3: Search for types/methods that reference project IDs
        Console.WriteLine("\n=== Types with 'GetProject' methods ===");
        foreach (var t in types)
        {
            try
            {
                var methods = t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                foreach (var m in methods)
                {
                    if (m.Name.Contains("Project") || m.Name.Contains("BiosId"))
                        Console.WriteLine("  " + t.FullName + "." + m.Name + "()");
                }
            }
            catch { }
        }

        // PART 4: Search for types that have fields with model-specific data
        Console.WriteLine("\n=== Types containing 'Model' or 'Platform' or 'Spec' in name ===");
        foreach (var t in types)
        {
            try
            {
                if (t.Name.Contains("Model") || t.Name.Contains("Platform") ||
                    (t.Name.Contains("Spec") && !t.Name.Contains("ECSpec")))
                    Console.WriteLine("  " + t.FullName + " (Abstract=" + t.IsAbstract + ")");
            }
            catch { }
        }

        // PART 5: Look for where project-specific branching happens
        Console.WriteLine("\n=== Types with 'Fan' + 'Table' or 'Profile' in name ===");
        foreach (var t in types)
        {
            try
            {
                if ((t.Name.Contains("Fan") && t.Name.Contains("Table")) ||
                    t.Name.Contains("FanProfile") || t.Name.Contains("FanSpec"))
                    Console.WriteLine("  " + t.FullName);
            }
            catch { }
        }

        Console.WriteLine("\nDone.");
    }
}
