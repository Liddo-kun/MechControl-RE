using System;
using System.Reflection;
using System.Linq;
using System.IO;
using System.Collections.Generic;

class Program
{
    static void Main()
    {
        // Resolve dependencies automatically
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
        Console.WriteLine("Loading: " + gcuPath);

        Assembly asm = Assembly.ReflectionOnlyLoadFrom(gcuPath);

        Type[] types;
        try { types = asm.GetTypes(); }
        catch (ReflectionTypeLoadException ex)
        {
            types = ex.Types.Where(t => t != null).ToArray();
        }
        Console.WriteLine("Total types: " + types.Length);

        // Find ALL types that inherit from any ECSpec
        string[] ecSpecNames = new string[] { "ECSpec", "RamFan1_ECSpec", "RamFan1p5_ECSpec", "RamFan2_ECSpec" };

        Console.WriteLine("\n=== Searching for ECSpec subclasses ===");
        var subclasses = new List<Type>();
        foreach (var t in types)
        {
            try
            {
                Type bt = t.BaseType;
                if (bt != null && ecSpecNames.Contains(bt.Name))
                {
                    subclasses.Add(t);
                    Console.WriteLine("  FOUND: " + t.FullName + " extends " + bt.Name + " (Abstract=" + t.IsAbstract + ")");
                }
            }
            catch { }
        }
        Console.WriteLine("Total subclasses: " + subclasses.Count);

        // Also check 2nd level (subclass of subclass)
        Console.WriteLine("\n=== Checking for 2nd-level subclasses ===");
        var level2 = new List<Type>();
        var subNames = subclasses.Select(s => s.Name).ToList();
        foreach (var t in types)
        {
            try
            {
                Type bt = t.BaseType;
                if (bt != null && subNames.Contains(bt.Name) && !subclasses.Contains(t))
                {
                    level2.Add(t);
                    Console.WriteLine("  FOUND L2: " + t.FullName + " extends " + bt.Name + " (Abstract=" + t.IsAbstract + ")");
                }
            }
            catch { }
        }

        // Now dump fields for the base ECSpec class
        foreach (var t in types.Where(t2 => t2.Name == "ECSpec" && t2.Namespace == "Define"))
        {
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            Console.WriteLine("\n=== BASE: " + t.FullName + " : " + fields.Length + " fields ===");
            int constCount = 0;
            int nonConstCount = 0;
            foreach (var f in fields)
            {
                if (f.IsLiteral) constCount++;
                else nonConstCount++;
            }
            Console.WriteLine("  Const fields: " + constCount + ", Non-const: " + nonConstCount);

            // Show first 20 fields to understand the pattern
            foreach (var f in fields.Take(20))
            {
                string val = "?";
                if (f.IsLiteral)
                {
                    try
                    {
                        object raw = f.GetRawConstantValue();
                        if (raw is int || raw is uint || raw is ushort || raw is byte)
                            val = "0x" + Convert.ToInt32(raw).ToString("X");
                        else
                            val = raw != null ? raw.ToString() : "null";
                    }
                    catch { val = "error"; }
                }
                else
                {
                    val = "(" + (f.IsStatic ? "static" : "instance") + " " + (f.IsInitOnly ? "readonly" : "mutable") + ")";
                }
                Console.WriteLine("    " + f.FieldType.Name + " " + f.Name + " = " + val);
            }
            if (fields.Length > 20)
                Console.WriteLine("    ... " + (fields.Length - 20) + " more");
        }

        // For each subclass, show how many fields it DECLARES (overrides/adds)
        Console.WriteLine("\n=== Subclass field counts ===");
        foreach (var t in subclasses)
        {
            try
            {
                var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
                int constCount = fields.Count(f => f.IsLiteral);
                Console.WriteLine("  " + t.Name + ": " + fields.Length + " declared fields (" + constCount + " const)");
            }
            catch (Exception ex) { Console.WriteLine("  " + t.Name + ": ERROR " + ex.Message); }
        }

        // Dump first subclass fully as a sample
        if (subclasses.Count > 0)
        {
            // Find one named IDY or first available
            Type sample = subclasses.FirstOrDefault(t2 => t2.Name.Contains("IDY"));
            if (sample == null) sample = subclasses[0];

            var sFields = sample.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            Console.WriteLine("\n=== SAMPLE: " + sample.FullName + " : " + sFields.Length + " fields ===");
            foreach (var f in sFields)
            {
                string val = "?";
                if (f.IsLiteral)
                {
                    try
                    {
                        object raw = f.GetRawConstantValue();
                        if (raw is int || raw is uint || raw is ushort || raw is byte)
                            val = "0x" + Convert.ToInt32(raw).ToString("X");
                        else
                            val = raw != null ? raw.ToString() : "null";
                    }
                    catch { val = "error"; }
                }
                else
                {
                    val = "(" + (f.IsStatic ? "static" : "instance") + " " + (f.IsInitOnly ? "readonly" : "mutable") + ")";
                }
                Console.WriteLine("    " + f.FieldType.Name + " " + f.Name + " = " + val);
            }
        }
    }
}
