using System;
using System.Reflection;
using System.Linq;
using System.IO;
using System.Collections.Generic;

class Program
{
    static void Main()
    {
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
            Console.WriteLine("Warning: partial load, got " + types.Length + " types");
            foreach (Exception le in ex.LoaderExceptions.Take(5))
                Console.WriteLine("  LoaderEx: " + le.Message);
        }

        Console.WriteLine("Total types loaded: " + types.Length);

        // Find ECSpec-related types
        var ecTypes = types.Where(t => t.Name.Contains("ECSpec")).ToList();

        Console.WriteLine("\n=== ECSpec types found: " + ecTypes.Count + " ===");
        foreach (var t in ecTypes)
        {
            string baseName = t.BaseType != null ? t.BaseType.Name : "null";
            Console.WriteLine("  " + t.FullName + " (Abstract=" + t.IsAbstract + ", Base=" + baseName + ")");
        }

        // All types in Define namespace
        var defineTypes = types.Where(t => t.Namespace != null && t.Namespace.Contains("Define")).ToList();
        Console.WriteLine("\n=== All types in Define namespace: " + defineTypes.Count + " ===");
        foreach (var t in defineTypes)
        {
            Console.WriteLine("  " + t.FullName);
        }

        // For each ECSpec type, list fields
        foreach (var t in ecTypes)
        {
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            Console.WriteLine("\n--- " + t.FullName + " : " + fields.Length + " fields ---");
            foreach (var f in fields.Take(40))
            {
                string val = "?";
                if (f.IsLiteral)
                {
                    try
                    {
                        object raw = f.GetRawConstantValue();
                        val = raw != null ? raw.ToString() : "null";
                    }
                    catch (Exception ex) { val = "error: " + ex.Message; }
                }
                else
                {
                    val = "(not const, " + (f.IsStatic ? "static" : "instance") + ")";
                }
                Console.WriteLine("    " + f.Name + " : " + f.FieldType.Name + " = " + val);
            }
            if (fields.Length > 40)
                Console.WriteLine("    ... (" + (fields.Length - 40) + " more fields)");
        }
    }
}
