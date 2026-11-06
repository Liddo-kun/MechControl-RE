using System;
using System.Reflection;
using System.Linq;
using System.IO;

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

        // Dump ProjectID type
        foreach (var t in types.Where(t2 => t2.Name == "ProjectID"))
        {
            Console.WriteLine("=== " + t.FullName + " (IsEnum=" + t.IsEnum + ") ===");
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            foreach (var f in fields.OrderBy(f2 => f2.Name))
            {
                string val = "?";
                if (f.IsLiteral)
                {
                    try { val = f.GetRawConstantValue().ToString(); }
                    catch { val = "error"; }
                }
                Console.WriteLine("  " + f.Name + " = " + val);
            }
        }

        // Dump ProjectIdExtensions methods
        Console.WriteLine("\n=== ProjectIdExtensions methods ===");
        foreach (var t in types.Where(t2 => t2.Name == "ProjectIdExtensions"))
        {
            var methods = t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            foreach (var m in methods)
            {
                var parms = m.GetParameters();
                string pStr = "";
                foreach (var p in parms)
                    pStr += p.ParameterType.Name + " " + p.Name + ", ";
                Console.WriteLine("  " + m.ReturnType.Name + " " + m.Name + "(" + pStr.TrimEnd(',', ' ') + ")");
            }
        }

        // Dump FanModel types to understand model differentiation
        Console.WriteLine("\n=== FanModel types ===");
        foreach (var t in types.Where(t2 => t2.Namespace != null && t2.FullName.Contains("FanModel") && !t2.Name.Contains("<")))
        {
            Console.WriteLine("\n--- " + t.FullName + " ---");
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            foreach (var f in fields.Take(20))
            {
                string val = "?";
                if (f.IsLiteral)
                {
                    try { val = f.GetRawConstantValue().ToString(); }
                    catch { val = "error"; }
                }
                else
                {
                    val = "(" + (f.IsStatic ? "static" : "instance") + ")";
                }
                Console.WriteLine("  " + f.FieldType.Name + " " + f.Name + " = " + val);
            }
        }

        // Look for how battery protection works
        Console.WriteLine("\n=== Types with 'Battery' in name ===");
        foreach (var t in types.Where(t2 => t2.Name.Contains("Battery")))
        {
            Console.WriteLine("  " + t.FullName);
            var methods = t.GetMethods(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            foreach (var m in methods.Take(15))
                Console.WriteLine("    " + m.Name + "()");
        }

        // Look for display/local dimming
        Console.WriteLine("\n=== Types with 'Display' or 'Dimming' or 'LocalDimm' ===");
        foreach (var t in types.Where(t2 => t2.Name.Contains("Display") || t2.Name.Contains("Dimming") || t2.Name.Contains("LocalDimm")))
        {
            Console.WriteLine("  " + t.FullName);
        }
    }
}
