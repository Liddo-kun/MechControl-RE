using System;
using System.Runtime.InteropServices;
using System.Diagnostics;
class P {
    const uint GR=0x80000000,GW=0x40000000,OE=3,FS=3;
    const uint ECR=0x9C40A488,ECW=0x9C40A48C;
    [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Auto)]
    static extern IntPtr CreateFile(string f,uint a,uint s,IntPtr z,uint c,uint fl,IntPtr t);
    [DllImport("kernel32.dll",SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h,uint code,int[] i,uint iSz,ref int o,uint oSz,out uint ret,IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);
    static IntPtr hDev;
    static byte R(int a){int o=0;uint r;DeviceIoControl(hDev,ECR,new int[]{a},4,ref o,4,out r,IntPtr.Zero);return (byte)(o&0xFF);}
    static void W(int a,byte v){int[] i=new int[]{a,v};int o=0;uint r;DeviceIoControl(hDev,ECW,i,8,ref o,4,out r,IntPtr.Zero);}
    static void Main(){
        hDev=CreateFile(@"\\.\ACPIDriver",GR|GW,FS,IntPtr.Zero,OE,0,IntPtr.Zero);
        if(hDev==new IntPtr(-1)){Console.WriteLine("No driver");return;}
        try{
            var sw=Stopwatch.StartNew();
            byte before=R(0x7A6);
            W(0x7A6,0x08);
            byte imm=R(0x7A6);
            long t1=sw.ElapsedTicks;
            System.Threading.Thread.Sleep(1);
            byte after1=R(0x7A6);
            System.Threading.Thread.Sleep(10);
            byte after10=R(0x7A6);
            System.Threading.Thread.Sleep(100);
            byte after100=R(0x7A6);
            double usPerTick=1000000.0/Stopwatch.Frequency;
            Console.WriteLine("Before:    0x{0:X2}",before);
            Console.WriteLine("Wrote:     0x08");
            Console.WriteLine("Immediate: 0x{0:X2} ({1:F0}us later)",imm,t1*usPerTick);
            Console.WriteLine("+1ms:      0x{0:X2}",after1);
            Console.WriteLine("+10ms:     0x{0:X2}",after10);
            Console.WriteLine("+100ms:    0x{0:X2}",after100);
        }finally{CloseHandle(hDev);}
    }
}
