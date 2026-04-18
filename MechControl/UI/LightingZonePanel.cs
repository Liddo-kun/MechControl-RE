namespace MechControl.UI;

class LightingZonePanel : Section
{
    private readonly CheckBox _chkEnabled;
    private readonly UiHelpers.SliderRow _brightness;
    private readonly UiHelpers.RgbFlow _rgb;
    private readonly Button _btnApply;
    private readonly Button _btnPickColor;
    private readonly Button? _btnSave;

    public event EventHandler? ApplyClicked;
    public event EventHandler? SaveClicked;

    public bool ZoneEnabled
    {
        get => _chkEnabled.Checked;
        set => _chkEnabled.Checked = value;
    }

    public System.Drawing.Color ZoneColor
    {
        get => System.Drawing.Color.FromArgb(
            (int)_rgb.R.Value, (int)_rgb.G.Value, (int)_rgb.B.Value);
        set
        {
            _rgb.R.Value = value.R;
            _rgb.G.Value = value.G;
            _rgb.B.Value = value.B;
        }
    }

    public int ZoneBrightness
    {
        get => _brightness.Slider.Value;
        set => _brightness.Slider.Value = Math.Clamp(value, 0, 100);
    }

    public LightingZonePanel(string title, bool hasSaveButton,
        byte initialR = 0, byte initialG = 0, byte initialB = 0,
        int initialBrightness = 50, bool initialEnabled = false)
        : base(title, fitContent: false)
    {
        // Add each logical row directly to Section (which is itself a
        // ColumnCount=1 TableLayoutPanel). No inner wrapper TLP.

        _chkEnabled = new CheckBox
        {
            Text = "Enabled",
            AutoSize = true,
            Font = UiHelpers.FontNormal,
            Checked = initialEnabled,
            Margin = new Padding(0, 4, 0, UiMetrics.RowGap),
        };
        Controls.Add(_chkEnabled);

        var sliderRow = new TableLayoutPanel
        {
            Dock = DockStyle.Top,
            AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink,
            ColumnCount = 3,
            Margin = new Padding(0),
        };
        sliderRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        sliderRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        sliderRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _brightness = UiHelpers.BuildSliderRow(
            "Brightness", 0, 100, "%", initial: initialBrightness);
        UiHelpers.AddSliderRow(sliderRow, _brightness);
        Controls.Add(sliderRow);

        _rgb = UiHelpers.BuildRgbFlow(initialR, initialG, initialB);
        Controls.Add(_rgb.Flow);

        // Plain Panel with explicit Height AND Width. Width must be the same
        // across all zones — a narrower Panel (e.g. zones without a Save
        // button) re-triggers the parent Section's row-height bug and
        // leaves a vertical gap below the button row.
        const int BtnRowWidth =
            UiMetrics.ButtonApplyWidth * 2
            + UiMetrics.ButtonWideWidth
            + UiMetrics.ButtonGap * 2;
        var btnRow = new Panel
        {
            Height = UiMetrics.ButtonHeight,
            Width = BtnRowWidth,
            AutoSize = false,
            Margin = new Padding(0),
        };

        int x = 0;
        Button AddButton(Button b)
        {
            b.Location = new System.Drawing.Point(x, 0);
            btnRow.Controls.Add(b);
            x += b.Width + UiMetrics.ButtonGap;
            return b;
        }

        _btnApply = AddButton(UiHelpers.BuildApplyButton("Apply",
            (s, e) => ApplyClicked?.Invoke(this, EventArgs.Empty),
            margin: new Padding(0)));

        _btnPickColor = AddButton(UiHelpers.BuildApplyButton("Pick Color...",
            BtnPickColor_Click, margin: new Padding(0)));

        if (hasSaveButton)
        {
            _btnSave = AddButton(UiHelpers.BuildApplyButton("Save to EC",
                (s, e) => SaveClicked?.Invoke(this, EventArgs.Empty),
                width: UiMetrics.ButtonWideWidth,
                margin: new Padding(0)));
        }

        Controls.Add(btnRow);
    }

    private void BtnPickColor_Click(object? sender, EventArgs e)
    {
        using var dlg = new ColorDialog
        {
            Color = _rgb.Preview.BackColor,
            FullOpen = true,
        };
        if (dlg.ShowDialog() == DialogResult.OK)
        {
            _rgb.R.Value = dlg.Color.R;
            _rgb.G.Value = dlg.Color.G;
            _rgb.B.Value = dlg.Color.B;
        }
    }
}
