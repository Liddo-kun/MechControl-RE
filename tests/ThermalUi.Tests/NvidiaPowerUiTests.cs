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
        var input = Field<UiHelpers.SliderRow>(tab, "_rowBoardInput");
        var unlock = Field<CheckBox>(tab, "_chkTgpUnlock");
        // Rails in whole amps (rounded down); an unmoved slider shows (and keeps) the exact detected value.
        Require(core.Slider.Value == 187 && core.Value.Text == "187.5 A" && fabric.Slider.Value == 57, "Current sliders did not populate detected values");
        // Board input positions are milliamps, arrow keys 0.1 A; never below 1 A.
        Require(core.Slider.Maximum == 252 && fabric.Slider.Maximum == 77 && input.Slider.Maximum == 15474 && core.Slider.Minimum == 1 && input.Slider.Minimum == 1000,
            "The three current ranges did not follow the shared 140%-of-default policy");
        Require(input.Slider.SmallChange == 100 && core.Slider.SmallChange == 1, "Arrow-key steps are not 0.1 A for board input and 1 A for the rails");
        Require(input.Slider.Value == 11053 && input.Value.Text == "11.053 A", "Board input lost fractional stock milliamps");
        var status = Field<Label>(tab, "_lblDriverLimits");
        Require(status.Text.Contains("stock currents 180 / 55 / 11.053 A"), "Status did not list the stock board input current");
        Require(unlock.Checked && backend.Configurations == 0, "Refreshing the checkbox caused hardware writes");
        Require(Field<TrackBar>(tab, "_sliderTgp").Maximum == 200, "TGP range did not follow alternate model/BIOS limits");
        void Apply() => typeof(PowerTab).GetMethod("BtnApply_Click", Private)!.Invoke(tab, new object?[] { null, EventArgs.Empty });
        Apply();
        Require(backend.Configurations == 0 && backend.Core == 187500 && backend.Input == 11053, "Applying unmoved current sliders rewrote or rounded them");
        core.Slider.Value = 186;
        Apply();
        Require(backend.Configurations == 1 && backend.Core == 186000 && backend.Fabric == 57000 && backend.Input == 11053,
            "Apply did not write the selected whole-amp current only");
        Require(core.Slider.Value == 186 && core.Value.Text == "186 A", "Readback after apply did not show the new current");
        var saved = new GpuProfileSettings { Tgp = 125 };
        gpu.CaptureDriverSettings(saved);
        Require(saved.NvvddMilliamps == 186000 && saved.MsvddMilliamps == 57000 && saved.BoardInputMilliamps == 11053 && saved.BiosOverrideEnabled == false,
            "Profile capture lost current units or asked for an unneeded unlock");
        // An already-unlocked user can raise the new input slider: one restart, then live changes.
        GpuProfileSettings Selected() => (GpuProfileSettings)typeof(PowerTab).GetMethod("ReadGpuSettings", Private)!.Invoke(tab, null)!;
        input.Slider.Value = 12300;
        Require(input.Value.Text == "12.3 A", "Board input did not show the selected current");
        var restartPlan = gpu.PlanProfile(Selected(), automatic: false);
        Require(restartPlan.Restart, "Raising board input above the old ceiling did not request a restart");
        gpu.ConfigureProfile(restartPlan); // Exercise the confirmed plan without opening a modal dialog.
        tab.RefreshFromEc();
        Require(backend.Input == 12300 && backend.Restarts == 1 && backend.Core == 186000 && backend.Fabric == 57000,
            "Board input did not upgrade its old ceiling without changing the other currents");
        input.Slider.Value = 15123;
        Apply();
        Require(backend.Input == 15123 && backend.Restarts == 1, "Board input restarted inside the active range");
        gpu.CaptureDriverSettings(saved);
        Require(saved.BoardInputMilliamps == 15123 && saved.BiosOverrideEnabled == true, "Raised board input alone did not persist its unlock requirement");
        backend.HasBoardInput = false;
        tab.RefreshFromEc();
        Require(!input.Slider.Enabled && input.Value.Text == "--" && core.Slider.Enabled, "An absent board policy disabled other controls or left its slider usable");
        Require(status.Text.Contains("stock currents 180 / 55 A"), "Status listed an absent board input current");
        backend.HasBoardInput = true;
        // Locked at stock ceilings, a fractional rail at its maximum (60.6 A) rounds down onto the
        // whole-amp maximum instead of being clamped below it; unmoved, nothing is written.
        backend.Enabled = false; backend.Input = 11053; backend.Fabric = backend.FabricStock = 60600;
        tab.RefreshFromEc();
        Require(fabric.Slider.Maximum == 60 && fabric.Slider.Value == 60 && fabric.Value.Text == "60.6 A" && Selected().MsvddMilliamps == null,
            "An unmoved fractional rail at its ceiling would be rewritten");
        Require(input.Slider.Maximum == 11053 && input.Slider.Value == 11053 && Selected().BoardInputMilliamps == null,
            "An unmoved board input slider at its stock ceiling would be rewritten");
        fabric.Slider.Value = 59;
        Require(fabric.Value.Text == "59 A" && Selected().MsvddMilliamps == 59000, "Moving the rail slider did not select its whole amp");
        backend.Enabled = true; backend.Fabric = 57000; backend.FabricStock = 60000;
        tab.RefreshFromEc();
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
        public bool Enabled { get; set; } = true;
        public int StockPowerLimitWatts => 150;
        public int MinimumPowerLimitWatts => 5;
        public GpuPowerLimits ReadPowerLimits() => new(225, 80, Requested);
        public int? Requested;
        public int Core = 187500, Fabric = 57000, FabricStock = 60000, Input = 11053, Configurations, Restarts;
        public bool HasBoardInput = true;
        private int InputMaximum => Restarts == 0 ? 11053 : 15474;
        public GpuCurrentLimits ReadCurrentLimits() => new(new(1, 180000, 220000, Core, 200000), new(1, 55000, 65000, Fabric, FabricStock),
            HasBoardInput ? new(1, 11053, InputMaximum, Input, 11053) : null);
        public bool RequiresRestart(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, int? boardInputMilliamps) =>
            nvvddMilliamps > 220000 || msvddMilliamps > 65000 || boardInputMilliamps > InputMaximum;
        public void Configure(bool enabled, int? nvvddMilliamps, int? msvddMilliamps, int? boardInputMilliamps, bool allowRestart)
        {
            if (RequiresRestart(enabled, nvvddMilliamps, msvddMilliamps, boardInputMilliamps))
            { Require(allowRestart, "Unexpected automatic GPU restart"); Restarts++; }
            Configurations++; Core = nvvddMilliamps ?? Core; Fabric = msvddMilliamps ?? Fabric; Input = boardInputMilliamps ?? Input;
        }
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
