namespace MechControl.Hardware.PawnIo;

enum BlackwellGpuThermState { Unavailable, Pending, Available, Asleep }
class BlackwellGpuThermSample
{
    public BlackwellGpuThermState State => BlackwellGpuThermState.Unavailable;
    public double? HotspotC => null;
    public double?[]? ChannelTemperaturesC => null;
}
class BlackwellGpuThermProvider
{
    public BlackwellGpuThermSample Poll() => new();
}
