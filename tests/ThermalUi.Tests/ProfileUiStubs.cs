using MechControl.Profile;

// Non-thermal devices are inert in the real ProfileBar/FanTab orchestration tests.
namespace MechControl.Features
{
    class BatteryControl
    {
        public int ReadBatteryMode() => 0;
        public void SetBatteryMode(int mode) { }
    }
    class LocalDimmingControl
    {
        public bool ReadState() => false;
        public void SetState(bool enabled) { }
    }
    class LightingControl
    {
        public const int ZoneLogo = 3, ZoneKeyboard = 0, ZoneLightBar = 2;
        public bool IsZoneAvailable(int zone) => false;
        public void SetZoneOn(int zone, byte r, byte g, byte b, int brightness) { }
        public void SetZoneOff(int zone) { }
        public (bool, byte, byte, byte, int) ReadLogoState() => (false, 0, 0, 0, 0);
    }
    class ZoneState
    {
        public bool Enabled { get; set; }
        public byte R { get; set; }
        public byte G { get; set; }
        public byte B { get; set; }
        public int Brightness { get; set; }
    }
    class LightingState
    {
        public ZoneState Logo { get; } = new();
        public ZoneState Keyboard { get; } = new();
        public ZoneState LightBar { get; } = new();
        public int IdleTimerSeconds { get; set; }
        public void Save() { }
        public static LightingState Load() => new();
    }
}

namespace MechControl.UI
{
    class LiquidCoolerTab
    {
        public void ApplySettings(LiquidCoolerProfileSettings settings) { }
        public LiquidCoolerProfileSettings ReadSettings() => new();
        public Task ApplyCurrentAsync() => Task.CompletedTask;
    }
}
