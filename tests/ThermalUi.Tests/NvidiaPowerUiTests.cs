using MechControl.Driver;
using MechControl.Features;
using MechControl.Profile;
using MechControl.UI;

static partial class Program
{
    private static void NvidiaPowerUiTests(bool capture = false)
    {
        var model = new MachineProfile();
        foreach (var (name, address) in new[] { ("PL1", "0x783"), ("PL2", "0x784"), ("PL4", "0x785"),
            ("cpuTempLimitWrite", "0x786"), ("tgpDbCtrl", "0x743"), ("tgp", "0x744"), ("dbTotalPower", "0x745"), ("dbMaxTgp", "0x746") })
            model.Registers[name] = address;
        model.Limits["TGP"] = new() { Min = 80, Max = 125, Offset = 70, Encoding = "offset" };
        UniversalDefaults.ApplyToProfile(model);

        // Not opened yet (stock user): unlock offered, current sliders disabled, NVIDIA untouched.
        var closedEc = new EcDevice();
        var closed = new GpuPowerControl(closedEc, new WriteGuard(closedEc, model), model) { OpenPowerOverride = () => throw new Exception("opened") };
        using (var stockTab = new PowerTab(null, closed, model))
        {
            stockTab.RefreshFromEc();
            Require(!Field<CheckBox>(stockTab, "_chkTgpUnlock").Checked && !Field<UiHelpers.SliderRow>(stockTab, "_rowNvvdd").Slider.Enabled,
                "Unopened NVIDIA controls were not offered as locked/disabled");
            Require(closed.OpenPowerOverride != null, "Building the Power tab initialized NVIDIA");
        }

        var ec = new EcDevice();
        ec.Registers[0x744] = 10; // The model minimum, 80 W.
        var backend = new UiGpuPower();
        var gpu = new GpuPowerControl(ec, new WriteGuard(ec, model), model) { PowerOverride = backend };
        using var tab = new PowerTab(null, gpu, model);
        tab.RefreshFromEc();
        var core = Field<UiHelpers.SliderRow>(tab, "_rowNvvdd");
        var fabric = Field<UiHelpers.SliderRow>(tab, "_rowMsvdd");
        var unlock = Field<CheckBox>(tab, "_chkTgpUnlock");
        // Whole-amp steps; an unmoved slider shows (and keeps) the exact detected value.
        Require(core.Slider.Value == 188 && core.Value.Text == "187.5 A" && fabric.Slider.Value == 57, "Current sliders did not populate detected values");
        Require(core.Slider.Maximum == 234 && core.Slider.Minimum == 1, "Current slider range did not follow the unlocked 130%-of-default policy");
        Require(unlock.Checked && backend.Configurations == 0, "Refreshing the checkbox caused hardware writes");
        Require(Field<TrackBar>(tab, "_sliderTgp").Maximum == 200, "TGP range did not follow alternate model/BIOS limits");
        void Apply() => typeof(PowerTab).GetMethod("BtnApply_Click", Private)!.Invoke(tab, new object?[] { null, EventArgs.Empty });
        Apply();
        Require(backend.Configurations == 0 && backend.Core == 187500, "Applying unmoved current sliders rewrote or rounded them");
        core.Slider.Value = 186;
        Apply();
        Require(backend.Configurations == 1 && backend.Core == 186000 && backend.Fabric == 57000, "Apply did not write the selected whole-amp current only");
        Require(core.Slider.Value == 186 && core.Value.Text == "186 A", "Readback after apply did not show the new current");
        var saved = new GpuProfileSettings { Tgp = 125 };
        gpu.CaptureDriverSettings(saved);
        Require(saved.NvvddMilliamps == 186000 && saved.MsvddMilliamps == 57000 && saved.BiosOverrideEnabled == false,
            "Profile capture lost current units or asked for an unneeded unlock");
        // Unlocked: TGP below the EC minimum (60% of 80 W) is a driver request; the EC stays at its minimum.
        var tgp = Field<TrackBar>(tab, "_sliderTgp");
        Require(tgp.Minimum == 48, "Unlocked TGP floor was not 60% of the model minimum");
        tgp.Value = 60;
        Apply();
        Require(ec.Registers[0x744] == 10 && backend.Requested == 60, "Sub-minimum TGP did not keep the EC at its minimum and lower the driver request");
        Require(tgp.Value == 60, "Readback did not show the sub-minimum TGP");
        var low = new GpuProfileSettings { Tgp = 60 };
        gpu.CaptureDriverSettings(low);
        Require(low.BiosOverrideEnabled == true, "A sub-minimum profile did not ask for the unlock");
        if (!capture) return;
        using var form = new Form { ClientSize = new Size(800, 850), ShowInTaskbar = false };
        form.Controls.Add(tab); form.Show(); Application.DoEvents(); form.PerformLayout();
        using var image = new Bitmap(800, 850);
        form.DrawToBitmap(image, new Rectangle(0, 0, 800, 850));
        Directory.CreateDirectory(@"C:\Temp\tandem\thermal-ui-validation");
        image.Save(@"C:\Temp\tandem\thermal-ui-validation\nvidia-power.png");
    }

    private sealed class UiGpuPower : IGpuPowerOverride
    {
        public bool Enabled => true;
        public int StockPowerLimitWatts => 150;
        public int MinimumPowerLimitWatts => 5;
        public GpuPowerLimits ReadPowerLimits() => new(225, 80, Requested);
        public int? Requested;
        public int Core = 187500, Fabric = 57000, Configurations;
        public GpuCurrentLimits ReadCurrentLimits() => new(new(1, 180000, 220000, Core, 200000), new(1, 55000, 65000, Fabric, 60000));
        public bool RequiresRestart(bool enabled, int? nvvddMilliamps, int? msvddMilliamps) => nvvddMilliamps > 220000 || msvddMilliamps > 65000;
        public void Configure(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, bool allowRestart)
        { Configurations++; Core = nvvddMilliamps ?? Core; Fabric = msvddMilliamps ?? Fabric; }
        public void ApplyPowerLimit(int watts) => Requested = watts;
        public bool IsPowerLimitAtMost(int watts) => true;
        public bool IsPowerLimitApplied(int watts) => Requested == watts;
        public GpuPowerBudget ReadPowerBudget() => new(175, 350);
        public bool IsGpuAwake() => true;
    }
}

namespace MechControl.Features
{
    // Power-tab tests exercise rendering/orchestration with already-validated EC capabilities.
    sealed class StartupValidator { public bool IsFeatureEnabled(string feature) => true; }
}
