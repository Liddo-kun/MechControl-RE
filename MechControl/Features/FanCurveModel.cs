using MechControl.Driver;

namespace MechControl.Features;

class FanCurvePoint
{
    public int TempUp;
    public int TempDown;
    public int DutyPercent;
}

class FanCurveModel
{
    /// <summary>
    /// Hysteresis range in °C. The EC evaluates the fan table with the
    /// "right-point" rule: duty[i] applies when tempUp[i-1] &lt; temp ≤ tempUp[i].
    /// Crossing tempUp[i] going up jumps duty to duty[i+1]; crossing
    /// tempDown[i] going down drops duty to duty[i-1]. We pair this with a
    /// cross-point tempDown formula so every zone gets a clean RangeCelsius-
    /// wide hysteresis band: tempDown[i] = tempUp[i-1] - RangeCelsius (for
    /// i ≥ 1). Also doubles as the minimum spacing enforced between adjacent
    /// tempUp values.
    /// </summary>
    public const int RangeCelsius = 4;

    public FanCurvePoint[] Points;
    public int ActiveEntries => Points.Length;

    public FanCurveModel(int activeEntries)
    {
        Points = new FanCurvePoint[activeEntries];
        for (int i = 0; i < activeEntries; i++)
            Points[i] = new FanCurvePoint();
    }

    /// <summary>
    /// Detect active fan point count by scanning tempUp for first 0xFF (padding).
    /// Max real temp is 105C, so 0xFF is always padding.
    /// Returns fallback if detection yields 0 (uninitialized table).
    /// </summary>
    public static int DetectActiveEntries(EcDevice device, int tempUpBase, int entriesPerArray, int fallback)
    {
        for (int i = 0; i < entriesPerArray; i++)
        {
            if (device.ReadByte(tempUpBase + i) >= 254) // 254 or 255 = padding
                return i > 0 ? i : fallback;
        }
        return fallback;
    }

    public static FanCurveModel ReadFromEc(EcDevice device, int tempUpBase, int tempDownBase, int dutyBase,
        int entriesPerArray, int fallbackActiveEntries)
    {
        int active = DetectActiveEntries(device, tempUpBase, entriesPerArray, fallbackActiveEntries);
        var model = new FanCurveModel(active);
        for (int i = 0; i < active; i++)
        {
            model.Points[i].TempUp = device.ReadByte(tempUpBase + i);
            model.Points[i].TempDown = i > 0 ? device.ReadByte(tempDownBase + i) : 0;
            model.Points[i].DutyPercent = device.ReadByte(dutyBase + i) / 2; // raw_div_2_percent
        }
        // EC requires duty[0] = 0x00 — force it regardless of what the EC had
        if (active > 0) model.Points[0].DutyPercent = 0;
        return model;
    }

    public List<string> Validate()
    {
        var errors = new List<string>();

        if (Points.Length > 0 && Points[0].DutyPercent != 0)
            errors.Add($"Point 0: duty must be 0% (EC rejects table if duty[0] != 0), got {Points[0].DutyPercent}%");

        for (int i = 0; i < Points.Length - 1; i++)
        {
            if (Points[i].TempUp + RangeCelsius > Points[i + 1].TempUp)
                errors.Add($"Point {i}: tempUp ({Points[i].TempUp}) must be at least {RangeCelsius}\u00B0C below point {i + 1} tempUp ({Points[i + 1].TempUp})");

            if (Points[i].DutyPercent > Points[i + 1].DutyPercent)
                errors.Add($"Point {i}: duty ({Points[i].DutyPercent}%) must be <= point {i + 1} duty ({Points[i + 1].DutyPercent}%)");
        }

        return errors;
    }

    public FanCurveModel Clone()
    {
        var clone = new FanCurveModel(ActiveEntries);
        for (int i = 0; i < ActiveEntries; i++)
        {
            clone.Points[i].TempUp = Points[i].TempUp;
            clone.Points[i].TempDown = Points[i].TempDown;
            clone.Points[i].DutyPercent = Points[i].DutyPercent;
        }
        return clone;
    }

    /// <summary>
    /// Recompute every TempDown from the current TempUp values using the
    /// cross-point formula tempDown[i] = tempUp[i-1] - RangeCelsius
    /// (point 0 pinned to 0; never written to EC). Call at every
    /// materialization point (EC write, profile serialize) so user-edited
    /// TempUp changes always produce a matching tempDown set.
    /// </summary>
    public void EnforceRange()
    {
        if (Points.Length == 0) return;
        Points[0].TempDown = 0;
        for (int i = 1; i < Points.Length; i++)
            Points[i].TempDown = Math.Max(0, Points[i - 1].TempUp - RangeCelsius);
    }

    /// <summary>
    /// Detect active entry count from a list of tempUp values.
    /// A valid active entry is in (0, 253] AND strictly greater than the previous entry.
    /// Stops at the first padding byte (>= 254), zero (uninitialized), or
    /// non-ascending value. Returns 0 when the table has no valid leading entries
    /// (e.g. all-zero cold-boot state or all-0xFF OEM-absent state).
    /// </summary>
    public static int DetectActiveEntriesFromList(IReadOnlyList<int> tempUp)
    {
        int prev = -1;
        for (int i = 0; i < tempUp.Count; i++)
        {
            int v = tempUp[i];
            if (v == 0 || v >= 254 || v <= prev)
                return i;
            prev = v;
        }
        return tempUp.Count;
    }

    /// <summary>
    /// Create a FanCurveModel from raw arrays (tempUp/tempDown/duty with padding).
    /// Duty values in the arrays are raw EC bytes (0-200), converted to percent (0-100).
    /// </summary>
    public static FanCurveModel FromRawArrays(IReadOnlyList<int> tempUp, IReadOnlyList<int> tempDown, IReadOnlyList<int> duty)
    {
        int active = DetectActiveEntriesFromList(tempUp);
        var model = new FanCurveModel(active);
        for (int i = 0; i < active; i++)
        {
            model.Points[i].TempUp = tempUp[i];
            model.Points[i].TempDown = i < tempDown.Count ? tempDown[i] : 0;
            model.Points[i].DutyPercent = i < duty.Count ? duty[i] / 2 : 0;
        }
        // EC requires duty[0] = 0x00 — force it regardless of source data
        if (active > 0) model.Points[0].DutyPercent = 0;
        return model;
    }

    /// <summary>
    /// Generic 10-point starter curve used as a last-resort cold-boot fallback
    /// when neither the EC snapshot nor a profile default is usable. Conservative
    /// ramp that's safe on any laptop; the user is expected to review and edit
    /// before applying. 10 points matches the typical active-entry count in
    /// real OEM fan tables, so the panels look familiar.
    /// </summary>
    public static FanCurveModel CreateGenericDefault()
    {
        int[] tempUp   = { 30, 40, 50, 60, 65, 70, 75, 80, 85, 90 };
        // tempDown[i] = tempUp[i-1] - RangeCelsius; index 0 is never written.
        int[] tempDown = {  0, 26, 36, 46, 56, 61, 66, 71, 76, 81 };
        int[] duty     = {  0, 25, 35, 45, 55, 65, 75, 85, 95, 100 };
        var model = new FanCurveModel(tempUp.Length);
        for (int i = 0; i < tempUp.Length; i++)
        {
            model.Points[i].TempUp = tempUp[i];
            model.Points[i].TempDown = tempDown[i];
            model.Points[i].DutyPercent = duty[i];
        }
        return model;
    }
}
