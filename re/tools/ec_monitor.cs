using System;
using System.Runtime.InteropServices;
using System.Threading;

class Program
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(
        IntPtr hDevice, uint dwIoControlCode,
        ref int lpInBuffer, int nInBufferSize,
        ref int lpOutBuffer, int nOutBufferSize,
        ref int lpBytesReturned, IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr hObject);

    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint OPEN_EXISTING = 0x3;
    const uint IOCTL_GPD_ACPI_ECREAD = 0x9C40A488;

    struct RegWatch
    {
        public int Addr;
        public string Name;
        public int LastValue;
    }

    static void Main()
    {
        Console.WriteLine("=== EC Register Monitor (READ ONLY) ===");
        Console.WriteLine("Polls registers every 2 seconds. Shows changes.");
        Console.WriteLine("Change settings in the OEM app and watch values change here.");
        Console.WriteLine("Press Ctrl+C to stop.");
        Console.WriteLine();

        IntPtr hDevice = CreateFile(@"\\.\ACPIDriver",
            GENERIC_READ | GENERIC_WRITE, 0x3,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);

        if (hDevice == new IntPtr(-1))
        {
            Console.WriteLine("FAILED to open device. Run as Administrator.");
            return;
        }

        RegWatch[] regs = new RegWatch[] {
            // Keyboard backlight
            new RegWatch { Addr = 0x78C, Name = "KB_Enable (SINGLEKBL_ENABLE)" },
            new RegWatch { Addr = 0x78E, Name = "KB_Power (SINGLEKBL_SUPPORTPOWER)" },
            new RegWatch { Addr = 0x769, Name = "KB_R (RGBKB_LEVEL_R)" },
            new RegWatch { Addr = 0x76A, Name = "KB_G (RGBKB_LEVEL_G)" },
            new RegWatch { Addr = 0x76B, Name = "KB_B (RGBKB_LEVEL_B)" },

            // Fan
            new RegWatch { Addr = 0x751, Name = "Fan_CtrlMode (MAFAN_CONTROL)" },
            new RegWatch { Addr = 0x75B, Name = "Fan_L_Duty" },
            new RegWatch { Addr = 0x75C, Name = "Fan_R_Duty" },
            // Fan RPM removed — too noisy (fluctuates constantly)
            // new RegWatch { Addr = 0x464, Name = "Fan_Main_RPM1" },
            // new RegWatch { Addr = 0x465, Name = "Fan_Main_RPM2" },
            // new RegWatch { Addr = 0x46C, Name = "Fan_Sec_RPM1" },
            // new RegWatch { Addr = 0x46B, Name = "Fan_Sec_RPM2" },

            // Power limits
            new RegWatch { Addr = 0x783, Name = "PL1" },
            new RegWatch { Addr = 0x784, Name = "PL2" },
            new RegWatch { Addr = 0x785, Name = "PL4" },
            new RegWatch { Addr = 0x738, Name = "CPU_TAU" },

            // GPU TGP
            new RegWatch { Addr = 0x743, Name = "TGP_DynBoost_CTRL" },
            new RegWatch { Addr = 0x744, Name = "TGP_Value" },
            new RegWatch { Addr = 0x745, Name = "DynBoost_TotalPwr" },
            new RegWatch { Addr = 0x746, Name = "DynBoost_MaxTGP" },
            new RegWatch { Addr = 0x78B, Name = "GPU_Setting" },

            // Battery
            new RegWatch { Addr = 0x7B9, Name = "Bat_ChargeLimit_Up" },
            new RegWatch { Addr = 0x7D0, Name = "Bat_ChargeLimit_Down" },
            new RegWatch { Addr = 0x4AB, Name = "Bat_SOC" },
            new RegWatch { Addr = 0x490, Name = "PowerSource" },

            // Mode / status
            new RegWatch { Addr = 0x740, Name = "ProjectID" },
            new RegWatch { Addr = 0x741, Name = "OEM_Byte/FanAlert" },
            new RegWatch { Addr = 0x742, Name = "Support5" },
            new RegWatch { Addr = 0x765, Name = "Support1" },
            new RegWatch { Addr = 0x766, Name = "Support2" },
            new RegWatch { Addr = 0x767, Name = "Trigger" },
            new RegWatch { Addr = 0x768, Name = "Status" },
            new RegWatch { Addr = 0x75D, Name = "Trigger2" },

            // Display / local dimming area
            new RegWatch { Addr = 0xD4E, Name = "ColorCalib_Support" },
            new RegWatch { Addr = 0xD4F, Name = "MiscSwitch2" },
            new RegWatch { Addr = 0xD00, Name = "OEM_SingleZone" },
        };

        // Initial read
        Console.WriteLine("--- Initial Values ---");
        for (int i = 0; i < regs.Length; i++)
        {
            int val = ReadEC(hDevice, regs[i].Addr);
            regs[i].LastValue = val;
            Console.WriteLine("  [0x" + regs[i].Addr.ToString("X3") + "] " +
                regs[i].Name.PadRight(30) + " = " + val + " (0x" + val.ToString("X2") + ")");
        }

        Console.WriteLine();
        Console.WriteLine("--- Monitoring for changes (every 2s) ---");

        while (true)
        {
            Thread.Sleep(2000);

            bool anyChange = false;
            for (int i = 0; i < regs.Length; i++)
            {
                int val = ReadEC(hDevice, regs[i].Addr);
                if (val != regs[i].LastValue)
                {
                    if (!anyChange)
                    {
                        Console.WriteLine();
                        Console.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss") + "] CHANGES DETECTED:");
                        anyChange = true;
                    }
                    Console.WriteLine("  [0x" + regs[i].Addr.ToString("X3") + "] " +
                        regs[i].Name.PadRight(30) + " : " +
                        regs[i].LastValue + " -> " + val +
                        " (0x" + regs[i].LastValue.ToString("X2") + " -> 0x" + val.ToString("X2") + ")");
                    regs[i].LastValue = val;
                }
            }

            if (!anyChange)
                Console.Write(".");
        }
    }

    static int ReadEC(IntPtr hDevice, int addr)
    {
        int inBuf = addr;
        int outBuf = 0;
        int ret = 0;
        bool ok = DeviceIoControl(hDevice, IOCTL_GPD_ACPI_ECREAD,
            ref inBuf, 4, ref outBuf, 4, ref ret, IntPtr.Zero);
        return ok ? (outBuf & 0xFF) : -1;
    }
}
