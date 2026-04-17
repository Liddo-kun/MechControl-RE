using System.Runtime.InteropServices;

namespace MechControl.Driver;

static class EcDriver
{
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr CreateFile(
        string lpFileName,
        uint dwDesiredAccess,
        uint dwShareMode,
        IntPtr lpSecurityAttributes,
        uint dwCreationDisposition,
        uint dwFlagsAndAttributes,
        IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(
        IntPtr hDevice,
        uint dwIoControlCode,
        ref int lpInBuffer,
        int nInBufferSize,
        ref int lpOutBuffer,
        int nOutBufferSize,
        out int lpBytesReturned,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool DeviceIoControl(
        IntPtr hDevice,
        uint dwIoControlCode,
        int[] lpInBuffer,
        int nInBufferSize,
        ref int lpOutBuffer,
        int nOutBufferSize,
        out int lpBytesReturned,
        IntPtr lpOverlapped);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    public const uint GENERIC_READ_WRITE = 0xC0000000;
    public const uint FILE_SHARE_RW = 0x3;
    public const uint OPEN_EXISTING = 0x3;
    public const uint IOCTL_ECREAD = 0x9C40A488;
    public const uint IOCTL_ECWRITE = 0x9C40A48C;
    public const uint IOCTL_SMAPCTABLE = 0x9C40A500;
    public static readonly IntPtr INVALID_HANDLE = new IntPtr(-1);
}
