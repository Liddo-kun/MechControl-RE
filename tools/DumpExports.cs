using System;
using System.IO;
using System.Text;

class DumpExports
{
    static int Main(string[] args)
    {
        string path = args.Length > 0 ? args[0] : @"C:\Windows\System32\nvapi64.dll";
        byte[] f = File.ReadAllBytes(path);
        int pe = BitConverter.ToInt32(f, 0x3C);
        bool is64 = BitConverter.ToUInt16(f, pe + 0x18) == 0x20B;
        int ddOff = pe + 0x18 + (is64 ? 0x70 : 0x60);
        uint exportRva = BitConverter.ToUInt32(f, ddOff);
        uint exportSize = BitConverter.ToUInt32(f, ddOff + 4);
        Console.WriteLine("Export dir RVA=0x" + exportRva.ToString("X") + " size=0x" + exportSize.ToString("X"));
        if (exportRva == 0) { Console.WriteLine("No exports."); return 0; }

        int expOff = RvaToOffset(f, pe, is64, exportRva);
        uint numFuncs = BitConverter.ToUInt32(f, expOff + 0x14);
        uint numNames = BitConverter.ToUInt32(f, expOff + 0x18);
        uint funcsRva = BitConverter.ToUInt32(f, expOff + 0x1C);
        uint namesRva = BitConverter.ToUInt32(f, expOff + 0x20);
        uint ordsRva  = BitConverter.ToUInt32(f, expOff + 0x24);
        int namesOff = RvaToOffset(f, pe, is64, namesRva);
        int ordsOff  = RvaToOffset(f, pe, is64, ordsRva);
        Console.WriteLine("Functions: " + numFuncs + ", Names: " + numNames);
        for (int i = 0; i < numNames; i++)
        {
            uint nameRva = BitConverter.ToUInt32(f, namesOff + i * 4);
            int noff = RvaToOffset(f, pe, is64, nameRva);
            int end = noff; while (f[end] != 0) end++;
            string name = Encoding.ASCII.GetString(f, noff, end - noff);
            ushort ord = BitConverter.ToUInt16(f, ordsOff + i * 2);
            Console.WriteLine("  " + name + " (ord " + ord + ")");
        }
        return 0;
    }

    static int RvaToOffset(byte[] f, int pe, bool is64, uint rva)
    {
        int numSections = BitConverter.ToUInt16(f, pe + 6);
        int secOff = pe + 0x18 + (is64 ? 0xF0 : 0xE0);
        for (int i = 0; i < numSections; i++)
        {
            uint vsize = BitConverter.ToUInt32(f, secOff + i * 40 + 8);
            uint vaddr = BitConverter.ToUInt32(f, secOff + i * 40 + 12);
            uint rawOff = BitConverter.ToUInt32(f, secOff + i * 40 + 20);
            if (rva >= vaddr && rva < vaddr + Math.Max(vsize, 1))
                return (int)(rawOff + (rva - vaddr));
        }
        return (int)rva;
    }
}
