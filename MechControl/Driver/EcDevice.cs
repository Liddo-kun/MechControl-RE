using System.ComponentModel;
using System.Runtime.InteropServices;

namespace MechControl.Driver;

sealed class EcDevice : IDisposable
{
    private IntPtr _handle;
    private readonly object _lock = new();
    private readonly int[] _writeBuf = new int[2];
    private bool _disposed;

    public EcDevice()
    {
        _handle = OpenHandle();
    }

    private static IntPtr OpenHandle()
    {
        IntPtr h = EcDriver.CreateFile(
            @"\\.\ACPIDriver",
            EcDriver.GENERIC_READ_WRITE,
            EcDriver.FILE_SHARE_RW,
            IntPtr.Zero,
            EcDriver.OPEN_EXISTING,
            0,
            IntPtr.Zero);

        if (h == EcDriver.INVALID_HANDLE)
        {
            int err = Marshal.GetLastWin32Error();
            throw new Win32Exception(err, $"Failed to open \\\\.\\.\\ACPIDriver (error {err}). Is the driver installed? Are you running as Administrator?");
        }

        return h;
    }

    public byte ReadByte(int addr)
    {
        lock (_lock)
        {
            try
            {
                return ReadByteInternal(addr);
            }
            catch
            {
                TryReconnect();
                return ReadByteInternal(addr);
            }
        }
    }

    public void WriteByte(int addr, byte value)
    {
        lock (_lock)
        {
            try
            {
                WriteByteInternal(addr, value);
            }
            catch
            {
                TryReconnect();
                WriteByteInternal(addr, value);
            }
        }
    }

    private byte ReadByteInternal(int addr)
    {
        int inBuf = addr;
        int outBuf = 0;
        bool ok = EcDriver.DeviceIoControl(
            _handle,
            EcDriver.IOCTL_ECREAD,
            ref inBuf, 4,
            ref outBuf, 4,
            out _, IntPtr.Zero);

        if (!ok)
        {
            int err = Marshal.GetLastWin32Error();
            throw new Win32Exception(err, $"EC read failed at 0x{addr:X3} (error {err})");
        }

        return (byte)(outBuf & 0xFF);
    }

    private void WriteByteInternal(int addr, byte value)
    {
        _writeBuf[0] = addr;
        _writeBuf[1] = value;
        int outBuf = 0;
        bool ok = EcDriver.DeviceIoControl(
            _handle,
            EcDriver.IOCTL_ECWRITE,
            _writeBuf, 8,
            ref outBuf, 4,
            out _, IntPtr.Zero);

        if (!ok)
        {
            int err = Marshal.GetLastWin32Error();
            throw new Win32Exception(err, $"EC write failed at 0x{addr:X3} value 0x{value:X2} (error {err})");
        }
    }

    /// <summary>
    /// Read ECSpec config via IOCTL_SMAPCTABLE. Input: [0xBB, index].
    /// Returns 4 bytes of model-specific EC configuration data.
    /// </summary>
    public uint ReadSmapcTable(int index)
    {
        lock (_lock)
        {
            int inBuf = 0xBB | (index << 8);
            int outBuf = 0;
            bool ok = EcDriver.DeviceIoControl(
                _handle,
                EcDriver.IOCTL_SMAPCTABLE,
                ref inBuf, 4,
                ref outBuf, 4,
                out _, IntPtr.Zero);

            if (!ok)
            {
                int err = Marshal.GetLastWin32Error();
                throw new Win32Exception(err, $"SMAPCTABLE read failed at index 0x{index:X2} (error {err})");
            }

            return (uint)outBuf;
        }
    }

    private void TryReconnect()
    {
        try { EcDriver.CloseHandle(_handle); } catch { }
        _handle = OpenHandle();
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            EcDriver.CloseHandle(_handle);
        }
    }
}
