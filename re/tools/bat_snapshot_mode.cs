using System;
using System.IO;
using System.Runtime.InteropServices;
class P {
    const uint GR=0x80000000,GW=0x40000000,OE=3,FS=3;
    const uint ECR=0x9C40A488;
    [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Auto)]
    static extern IntPtr CreateFile(string f,uint a,uint s,IntPtr z,uint c,uint fl,IntPtr t);
    [DllImport("kernel32.dll",SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h,uint code,byte[] i,uint iSz,byte[] o,uint oSz,out uint ret,IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);
    static IntPtr hDev;
    static byte R(int a){
        byte[] i=new byte[]{(byte)(a&0xFF),(byte)((a>>8)&0xFF),0,0},o=new byte[4];
        uint r;DeviceIoControl(hDev,ECR,i,4,o,4,out r,IntPtr.Zero);return o[0];
    }
    static void Main(string[] args){
        string label=args.Length>0?args[0]:"snapshot";
        hDev=CreateFile(@"\\.\ACPIDriver",GR|GW,FS,IntPtr.Zero,OE,0,IntPtr.Zero);
        if(hDev==new IntPtr(-1)){Console.WriteLine("Cannot open driver");return;}
        try{
            // Dump ranges: 0x400-0x4BF (battery), 0x7A0-0x7FF (config)
            int[][] ranges=new int[][]{ new int[]{0x400,0x4BF}, new int[]{0x7A0,0x7FF} };
            foreach(var rng in ranges){
                Console.Write("{0:X3}:", rng[0]);
                for(int a=rng[0];a<=rng[1];a++){
                    if(a>rng[0]&&(a-rng[0])%16==0) Console.Write("\n{0:X3}:",a);
                    Console.Write(" {0:X2}",R(a));
                }
                Console.WriteLine();
            }
        }finally{CloseHandle(hDev);}
    }
}
