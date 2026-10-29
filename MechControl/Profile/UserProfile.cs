using System.Text.Json.Serialization;
using MechControl.Features;

namespace MechControl.Profile;

class UserProfile
{
    [JsonIgnore]
    public bool RequiresSave { get; set; }

    [JsonPropertyName("name")]
    public string Name { get; set; } = "";

    [JsonPropertyName("projectId")]
    public string ProjectId { get; set; } = "";

    [JsonPropertyName("savedAt")]
    public DateTime SavedAt { get; set; }

    [JsonPropertyName("cpu")]
    public CpuProfileSettings Cpu { get; set; } = new();

    [JsonPropertyName("gpu")]
    public GpuProfileSettings Gpu { get; set; } = new();

    [JsonPropertyName("gpuFailsafe")]
    public GpuFailsafeSettings GpuFailsafe { get; set; } = new();

    [JsonPropertyName("fan")]
    public FanProfileSettings Fan { get; set; } = new();

    [JsonPropertyName("battery")]
    public BatteryProfileSettings Battery { get; set; } = new();

    [JsonPropertyName("lighting")]
    public LightingProfileSettings Lighting { get; set; } = new();

    [JsonPropertyName("liquidCooler")]
    public LiquidCoolerProfileSettings? LiquidCooler { get; set; }
}

class CpuProfileSettings
{
    [JsonPropertyName("pl1")] public int PL1 { get; set; }
    [JsonPropertyName("pl2")] public int PL2 { get; set; }
    [JsonPropertyName("pl4")] public int PL4 { get; set; }
    [JsonPropertyName("tempLimit")] public int TempLimit { get; set; }
}

class GpuProfileSettings
{
    [JsonPropertyName("tgp")] public int Tgp { get; set; }
    [JsonPropertyName("dynamicBoostEnabled")] public bool DynamicBoostEnabled { get; set; }
    [JsonPropertyName("dynamicBoostWatts")] public int DynamicBoostWatts { get; set; }
    // True when the saved values need the NVIDIA unlock. Loading never removes the unlock.
    [JsonPropertyName("biosOverrideEnabled")] public bool? BiosOverrideEnabled { get; set; }
    // Null on legacy profiles or when NVIDIA was not in use: leave currents alone.
    [JsonPropertyName("nvvddMilliamps")] public int? NvvddMilliamps { get; set; }
    [JsonPropertyName("msvddMilliamps")] public int? MsvddMilliamps { get; set; }
}

class FanProfileSettings
{
    [JsonPropertyName("boostEnabled")] public bool BoostEnabled { get; set; }
    [JsonPropertyName("curveEnabled")] public bool CurveEnabled { get; set; }
    [JsonPropertyName("switchSpeedEnabled")] public bool SwitchSpeedEnabled { get; set; }
    [JsonPropertyName("switchSpeedMs")] public int SwitchSpeedMs { get; set; } = 100;
    [JsonPropertyName("linkedEnabled")] public bool LinkedEnabled { get; set; }

    [JsonPropertyName("cpuCurve")] public FanCurveArrays CpuCurve { get; set; } = new();
    [JsonPropertyName("gpuCurve")] public FanCurveArrays GpuCurve { get; set; } = new();
}

class BatteryProfileSettings
{
    [JsonPropertyName("mode")] public int Mode { get; set; }
}

class LightingProfileSettings
{
    [JsonPropertyName("logoEnabled")] public bool LogoEnabled { get; set; }
    [JsonPropertyName("logoR")] public byte LogoR { get; set; }
    [JsonPropertyName("logoG")] public byte LogoG { get; set; }
    [JsonPropertyName("logoB")] public byte LogoB { get; set; }
    [JsonPropertyName("logoBrightness")] public int LogoBrightness { get; set; }

    [JsonPropertyName("keyboardEnabled")] public bool KeyboardEnabled { get; set; }
    [JsonPropertyName("keyboardR")] public byte KeyboardR { get; set; }
    [JsonPropertyName("keyboardG")] public byte KeyboardG { get; set; }
    [JsonPropertyName("keyboardB")] public byte KeyboardB { get; set; }
    [JsonPropertyName("keyboardBrightness")] public int KeyboardBrightness { get; set; }

    [JsonPropertyName("lightBarEnabled")] public bool LightBarEnabled { get; set; }
    [JsonPropertyName("lightBarR")] public byte LightBarR { get; set; }
    [JsonPropertyName("lightBarG")] public byte LightBarG { get; set; }
    [JsonPropertyName("lightBarB")] public byte LightBarB { get; set; }
    [JsonPropertyName("lightBarBrightness")] public int LightBarBrightness { get; set; }

    [JsonPropertyName("idleTimerSeconds")] public int IdleTimerSeconds { get; set; }

    [JsonPropertyName("localDimmingEnabled")] public bool LocalDimmingEnabled { get; set; }
}

class LiquidCoolerProfileSettings
{
    [JsonPropertyName("fanOn")] public bool FanOn { get; set; }
    [JsonPropertyName("fanDuty")] public int FanDuty { get; set; } = 50;
    [JsonPropertyName("pumpOn")] public bool PumpOn { get; set; }
    [JsonPropertyName("pumpDuty")] public int PumpDuty { get; set; } = 60;
    [JsonPropertyName("pumpVoltage")] public PumpVoltage PumpVoltage { get; set; } = PumpVoltage.V8;
    [JsonPropertyName("rgbOn")] public bool RgbOn { get; set; }
    [JsonPropertyName("r")] public byte R { get; set; }
    [JsonPropertyName("g")] public byte G { get; set; }
    [JsonPropertyName("b")] public byte B { get; set; }
    [JsonPropertyName("rgbMode")] public CoolerRgbMode RgbMode { get; set; } = CoolerRgbMode.Static;
}
