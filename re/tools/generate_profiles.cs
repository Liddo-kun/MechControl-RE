using System;
using System.Reflection;
using System.Linq;
using System.IO;
using System.Text;
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

        // Get app version
        var asmName = asm.GetName();
        string appVersion = asmName.Version.ToString();

        Type[] types;
        try { types = asm.GetTypes(); }
        catch (ReflectionTypeLoadException ex)
        {
            types = ex.Types.Where(t => t != null).ToArray();
        }

        // 1. Collect ProjectID enum
        var projectIds = new List<KeyValuePair<string, int>>();
        foreach (var t in types.Where(t2 => t2.Name == "ProjectID" && t2.IsEnum))
        {
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.Static);
            foreach (var f in fields)
            {
                if (f.Name == "value__") continue;
                try
                {
                    int val = Convert.ToInt32(f.GetRawConstantValue());
                    if (f.Name != "None")
                        projectIds.Add(new KeyValuePair<string, int>(f.Name, val));
                }
                catch { }
            }
        }
        projectIds.Sort((a, b) => a.Value.CompareTo(b.Value));
        Console.WriteLine("Found " + projectIds.Count + " project IDs");

        // 2. Collect ECSpec registers
        var registers = new List<KeyValuePair<string, string>>();
        foreach (var t in types.Where(t2 => t2.Name == "ECSpec" && t2.Namespace == "Define"))
        {
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance);
            foreach (var f in fields.OrderBy(f2 => f2.Name))
            {
                if (!f.IsLiteral) continue;
                try
                {
                    object raw = f.GetRawConstantValue();
                    string val = "";
                    if (raw is ushort) val = "0x" + ((ushort)raw).ToString("X3");
                    else if (raw is uint) val = "0x" + ((uint)raw).ToString("X");
                    else if (raw is int) val = "0x" + ((int)raw).ToString("X");
                    else if (raw is byte) val = "0x" + ((byte)raw).ToString("X2");
                    else val = raw.ToString();
                    registers.Add(new KeyValuePair<string, string>(f.Name, val));
                }
                catch { }
            }
        }

        // 3. Collect RamFan ECSpec variants
        var ramfan1 = new List<KeyValuePair<string, string>>();
        var ramfan1p5 = new List<KeyValuePair<string, string>>();
        var ramfan2 = new List<KeyValuePair<string, string>>();
        foreach (var t in types.Where(t2 => t2.Name.Contains("ECSpec") && t2.Name != "ECSpec"))
        {
            List<KeyValuePair<string, string>> target;
            if (t.Name.Contains("RamFan2")) target = ramfan2;
            else if (t.Name.Contains("RamFan1p5")) target = ramfan1p5;
            else target = ramfan1;

            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance);
            foreach (var f in fields.OrderBy(f2 => f2.Name))
            {
                if (!f.IsLiteral) continue;
                try
                {
                    object raw = f.GetRawConstantValue();
                    string val = "";
                    if (raw is ushort) val = "0x" + ((ushort)raw).ToString("X3");
                    else val = raw.ToString();
                    target.Add(new KeyValuePair<string, string>(f.Name, val));
                }
                catch { }
            }
        }

        // 4. Collect IOCTL codes
        var ioctls = new List<KeyValuePair<string, string>>();
        foreach (var t in types.Where(t2 => t2.Name == "AcpiCtrl"))
        {
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance);
            foreach (var f in fields.OrderBy(f2 => f2.Name))
            {
                if (!f.IsLiteral) continue;
                if (!f.Name.StartsWith("IOCTL") && !f.Name.StartsWith("GPD_TYPE")) continue;
                try
                {
                    object raw = f.GetRawConstantValue();
                    string val = "0x" + Convert.ToUInt32(raw).ToString("X8");
                    ioctls.Add(new KeyValuePair<string, string>(f.Name, val));
                }
                catch { }
            }
        }

        // 5. Collect battery enums
        var batteryModes = new List<KeyValuePair<string, int>>();
        var batteryCommands = new List<KeyValuePair<string, int>>();
        foreach (var t in types)
        {
            try
            {
                if (t.Name == "BatteryHealthProtection_Status" && t.IsEnum)
                {
                    foreach (var f in t.GetFields(BindingFlags.Public | BindingFlags.Static))
                    {
                        try { batteryModes.Add(new KeyValuePair<string, int>(f.Name, Convert.ToInt32(f.GetRawConstantValue()))); }
                        catch { }
                    }
                }
                if (t.Name == "Battery_Commands" && t.IsEnum)
                {
                    foreach (var f in t.GetFields(BindingFlags.Public | BindingFlags.Static))
                    {
                        try { batteryCommands.Add(new KeyValuePair<string, int>(f.Name, Convert.ToInt32(f.GetRawConstantValue()))); }
                        catch { }
                    }
                }
            }
            catch { }
        }

        // 6. Collect mode constants
        var modeConstants = new List<KeyValuePair<string, string>>();
        foreach (var t in types.Where(t2 => t2.Name == "ECSpec" && t2.Namespace == "Define"))
        {
            var fields = t.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance);
            foreach (var f in fields.OrderBy(f2 => f2.Name))
            {
                if (!f.IsLiteral) continue;
                if (f.Name.StartsWith("FAN_") || f.Name.StartsWith("TURBO_") || f.Name.StartsWith("POWER_"))
                {
                    if (!f.Name.StartsWith("FAN_LEVEL") && !f.Name.Contains("ADDR"))
                    {
                        try
                        {
                            string val = Convert.ToUInt32(f.GetRawConstantValue()).ToString();
                            modeConstants.Add(new KeyValuePair<string, string>(f.Name, val));
                        }
                        catch { }
                    }
                }
            }
        }

        // Create output directory
        string outDir = @"C:\Users\Jon\clean\Profiles\MECHREVO";
        Directory.CreateDirectory(outDir);

        // Generate one JSON per project ID
        foreach (var proj in projectIds)
        {
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("{");

            // Vendor metadata
            sb.AppendLine("  \"_vendor\": {");
            sb.AppendLine("    \"brand\": \"MECHREVO\",");
            sb.AppendLine("    \"appName\": \"GCUService.exe\",");
            sb.AppendLine("    \"appVersion\": \"" + appVersion + "\",");
            sb.AppendLine("    \"extractDate\": \"" + DateTime.Now.ToString("yyyy-MM-dd") + "\",");
            sb.AppendLine("    \"sourceFile\": \"GCUService.exe (from 机械革命控制中心)\"");
            sb.AppendLine("  },");
            sb.AppendLine();

            // Project ID
            sb.AppendLine("  \"projectId\": \"" + proj.Key + "\",");
            sb.AppendLine("  \"projectIdValue\": " + proj.Value + ",");
            sb.AppendLine();

            // Driver
            sb.AppendLine("  \"driver\": {");
            sb.AppendLine("    \"devicePath\": \"\\\\\\\\.\\\\ACPIDriver\",");
            sb.AppendLine("    \"sysFile\": \"UWACPIDriver.sys\",");
            sb.AppendLine("    \"hardwareId\": \"ACPI\\\\INOU0000\",");
            sb.AppendLine("    \"signedBy\": \"Uniwill Technology Inc.\",");
            sb.AppendLine("    \"framework\": \"KMDF 1.15\"");
            sb.AppendLine("  },");
            sb.AppendLine();

            // IOCTLs
            sb.AppendLine("  \"ioctl\": {");
            for (int i = 0; i < ioctls.Count; i++)
            {
                string comma = (i < ioctls.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + ioctls[i].Key + "\": \"" + ioctls[i].Value + "\"" + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            // EC Registers
            sb.AppendLine("  \"ecRegisters\": {");
            // Only ADDR_ fields
            var addrRegs = registers.Where(r => r.Key.StartsWith("ADDR_")).ToList();
            for (int i = 0; i < addrRegs.Count; i++)
            {
                string comma = (i < addrRegs.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + addrRegs[i].Key + "\": \"" + addrRegs[i].Value + "\"" + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            // Fan table registers - RamFan1
            sb.AppendLine("  \"fanTableRegisters_RamFan1\": {");
            for (int i = 0; i < ramfan1.Count; i++)
            {
                string comma = (i < ramfan1.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + ramfan1[i].Key + "\": \"" + ramfan1[i].Value + "\"" + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            // Fan table registers - RamFan1p5
            sb.AppendLine("  \"fanTableRegisters_RamFan1p5\": {");
            for (int i = 0; i < ramfan1p5.Count; i++)
            {
                string comma = (i < ramfan1p5.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + ramfan1p5[i].Key + "\": \"" + ramfan1p5[i].Value + "\"" + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            // Fan table registers - RamFan2
            sb.AppendLine("  \"fanTableRegisters_RamFan2\": {");
            for (int i = 0; i < ramfan2.Count; i++)
            {
                string comma = (i < ramfan2.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + ramfan2[i].Key + "\": \"" + ramfan2[i].Value + "\"" + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            // System registers (non-ADDR)
            sb.AppendLine("  \"systemRegisters\": {");
            var sysRegs = registers.Where(r => !r.Key.StartsWith("ADDR_") && !r.Key.StartsWith("FAN_") && !r.Key.StartsWith("TURBO_") && !r.Key.StartsWith("POWER_") && !r.Key.StartsWith("LEVEL_") && !r.Key.StartsWith("APP_") && !r.Key.StartsWith("Animation") && !r.Key.StartsWith("QKEY")).ToList();
            for (int i = 0; i < sysRegs.Count; i++)
            {
                string comma = (i < sysRegs.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + sysRegs[i].Key + "\": \"" + sysRegs[i].Value + "\"" + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            // Mode constants
            sb.AppendLine("  \"modeConstants\": {");
            for (int i = 0; i < modeConstants.Count; i++)
            {
                string comma = (i < modeConstants.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + modeConstants[i].Key + "\": " + modeConstants[i].Value + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            // Battery
            sb.AppendLine("  \"batteryHealthModes\": {");
            for (int i = 0; i < batteryModes.Count; i++)
            {
                string comma = (i < batteryModes.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + batteryModes[i].Key + "\": " + batteryModes[i].Value + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            sb.AppendLine("  \"batteryCommands\": {");
            for (int i = 0; i < batteryCommands.Count; i++)
            {
                string comma = (i < batteryCommands.Count - 1) ? "," : "";
                sb.AppendLine("    \"" + batteryCommands[i].Key + "\": " + batteryCommands[i].Value + comma);
            }
            sb.AppendLine("  },");
            sb.AppendLine();

            // Features and limits — TBD (requires method body decompilation)
            sb.AppendLine("  \"features\": {");
            sb.AppendLine("    \"_note\": \"Feature support flags are determined at runtime by project ID. Requires decompilation of method bodies to extract per-model.\",");
            sb.AppendLine("    \"fanControl\": \"unknown\",");
            sb.AppendLine("    \"gpuTgp\": \"unknown\",");
            sb.AppendLine("    \"dynamicBoost\": \"unknown\",");
            sb.AppendLine("    \"keyboardBacklight\": \"unknown\",");
            sb.AppendLine("    \"localDimming\": \"unknown\",");
            sb.AppendLine("    \"batteryProtection\": \"unknown\",");
            sb.AppendLine("    \"fanTableFormat\": \"unknown\"");
            sb.AppendLine("  },");
            sb.AppendLine();

            sb.AppendLine("  \"limits\": {");
            sb.AppendLine("    \"_note\": \"Value ranges are determined at runtime. Requires decompilation of method bodies or live EC reads to extract per-model.\"");
            sb.AppendLine("  }");

            sb.AppendLine("}");

            string outFile = Path.Combine(outDir, proj.Key + ".json");
            File.WriteAllText(outFile, sb.ToString(), Encoding.UTF8);
        }

        Console.WriteLine("Generated " + projectIds.Count + " profile JSONs in " + outDir);

        // List them
        foreach (string f in Directory.GetFiles(outDir, "*.json"))
            Console.WriteLine("  " + Path.GetFileName(f));
    }
}
