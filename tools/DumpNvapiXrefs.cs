using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

class DumpNvapiXrefs
{
    static byte[] f;
    static int pe;
    static bool is64;
    static int secOff, numSections;

    static int Main(string[] args)
    {
        string path = args.Length > 0 ? args[0] : @"C:\Users\Jon\code\Mechcontrol\hwmonitor_1.65.1\HWMonitor_x64.exe";
        f = File.ReadAllBytes(path);
        pe = BitConverter.ToInt32(f, 0x3C);
        is64 = BitConverter.ToUInt16(f, pe + 0x18) == 0x20B;
        numSections = BitConverter.ToUInt16(f, pe + 6);
        secOff = pe + 0x18 + (is64 ? 0xF0 : 0xE0);
        int ddOff = pe + 0x18 + (is64 ? 0x70 : 0x60);
        uint importRva = BitConverter.ToUInt32(f, ddOff + 8);

        // Walk import descriptors, find nvapi*.dll
        int desc = (int)RvaToOff(importRva);
        uint queryIatRva = 0, queryIatSlotRva = 0;
        for (int d = 0; d < 256; d++)
        {
            int dOff = desc + d * 20;
            uint nameRva = BitConverter.ToUInt32(f, dOff + 12);
            uint firstThunk = BitConverter.ToUInt32(f, dOff + 16);
            uint origThunk = BitConverter.ToUInt32(f, dOff + 0);
            if (nameRva == 0 && firstThunk == 0) break;
            string mod = ReadStr(RvaToOff(nameRva)).ToLowerInvariant();
            if (!mod.Contains("nvapi")) continue;
            Console.WriteLine("Imports " + mod);
            uint thunkRva = origThunk != 0 ? origThunk : firstThunk;
            for (int t = 0; t < 4096; t++)
            {
                int tOff = (int)RvaToOff(thunkRva) + t * 8;
                ulong val = BitConverter.ToUInt64(f, tOff);
                if (val == 0) break;
                if ((val & 0x8000000000000000UL) != 0) continue;
                string fn = ReadStr(RvaToOff((uint)val + 2));
                Console.WriteLine("  " + fn + " -> IAT slot RVA 0x" + (firstThunk + (uint)(t * 8)).ToString("X"));
                if (fn.Contains("QueryInterface")) queryIatSlotRva = firstThunk + (uint)(t * 8);
            }
        }
        if (queryIatSlotRva == 0) { Console.WriteLine("nvapi_QueryInterface not statically imported; falling back to B9-harvest."); }

        // Harvest every `mov ecx, imm32` (B9) in executable sections as candidate nvapi IDs.
        var ids = new Dictionary<uint, int>();
        for (int s = 0; s < numSections; s++)
        {
            uint rawOff = BitConverter.ToUInt32(f, secOff + s * 40 + 20);
            uint rawSize = BitConverter.ToUInt32(f, secOff + s * 40 + 16);
            uint characteristics = BitConverter.ToUInt32(f, secOff + s * 40 + 36);
            if ((characteristics & 0x20000000) == 0) continue;
            for (uint i = 0; i + 5 <= rawSize; i++)
            {
                if (f[rawOff + i] != 0xB9) continue;
                uint id = BitConverter.ToUInt32(f, (int)(rawOff + i + 1));
                if (id > 0x0000FFFF) ids[id] = ids.ContainsKey(id) ? ids[id] + 1 : 1;
            }
        }
        string outPath = Path.Combine(Path.GetDirectoryName(path), "nvapi-candidates.txt");
        using (var w = new StreamWriter(outPath))
            foreach (var kv in ids) w.WriteLine("0x" + kv.Key.ToString("X8"));
        Console.WriteLine("Wrote " + ids.Count + " candidates to " + outPath);
        Console.WriteLine("Contains known GetThermalSettings 0xE3640A56: " + ids.ContainsKey(0xE3640A56));
        return 0;
    }

    static string ReadStr(uint off)
    {
        int end = (int)off; while (f[end] != 0) end++;
        return Encoding.ASCII.GetString(f, (int)off, end - (int)off);
    }

    static uint RvaToOff(uint rva)
    {
        for (int i = 0; i < numSections; i++)
        {
            uint vsize = BitConverter.ToUInt32(f, secOff + i * 40 + 8);
            uint vaddr = BitConverter.ToUInt32(f, secOff + i * 40 + 12);
            uint rawOff = BitConverter.ToUInt32(f, secOff + i * 40 + 20);
            if (rva >= vaddr && rva < vaddr + Math.Max(vsize, 1)) return rawOff + (rva - vaddr);
        }
        return rva;
    }
}
