using MechControl.Driver;
using Microsoft.Win32;

namespace MechControl.Features;

/// <summary>
/// Keeps EC activation (0x741) alive for the app's lifetime.
///
/// The EC clears the activation byte on sleep/resume and certain firmware
/// events; once cleared it silently ignores PL/fan/GPU writes, which used to
/// mean "profile apply fails until app restart". Three detection paths feed
/// one serialized recovery routine:
///   1. WriteGuard verify-failure hook (write path — immediate)
///   2. PowerModeChanged resume hook (~3s settle delay, off the UI thread)
///   3. 30s spot-check timer (1 EC byte read when healthy; deliberately NOT
///      the OEM's 1.5s 0x7CC keepalive)
///
/// Recovery re-runs the EC init sequence, which is safe to repeat: it does
/// not touch the fan table, 0x7C5 bit 7, or 0x7C6 (see EcActivationControl).
/// After a successful re-activation, <see cref="Reactivated"/> fires so the
/// orchestration layer can re-apply the last used profile.
///
/// Model compatibility: uses only universal Uniwill registers via
/// EcActivationControl (0x741 is ADDR_AP_OEM_BYTE on every known model).
/// </summary>
sealed class EcActivationWatchdog : IDisposable
{
    private const int SpotCheckIntervalMs = 30_000;
    private const int ResumeSettleDelayMs = 3_000;

    private readonly EcActivationControl _activation;
    private readonly object _recoveryLock = new();
    private System.Threading.Timer? _timer;
    private DateTime _lastRecoveryUtc = DateTime.MinValue;
    private volatile bool _disposed;

    /// <summary>
    /// Raised (on an arbitrary thread) after activation was lost and
    /// successfully restored. Subscribers must marshal to the UI thread.
    ///
    /// WARNING: this fires while the internal recovery lock is held. Handlers
    /// must not block or perform synchronous EC writes — doing so can deadlock
    /// against a concurrent recovery path or re-enter recovery. Queue work
    /// asynchronously instead (e.g. Control.BeginInvoke), like the MainForm
    /// subscriber does.
    /// </summary>
    public event Action? Reactivated;

    public EcActivationWatchdog(EcActivationControl activation)
    {
        _activation = activation;
    }

    public void Start()
    {
        SystemEvents.PowerModeChanged += OnPowerModeChanged;
        _timer = new System.Threading.Timer(_ => SpotCheck("spot-check"), null,
            SpotCheckIntervalMs, SpotCheckIntervalMs);
    }

    private void OnPowerModeChanged(object sender, PowerModeChangedEventArgs e)
    {
        if (e.Mode != PowerModes.Resume) return;
        // Raised on the UI message loop — move off it and give the EC a
        // moment to settle after resume before probing.
        Task.Run(async () =>
        {
            await Task.Delay(ResumeSettleDelayMs);
            SpotCheck("resume");
        });
    }

    private void SpotCheck(string reason)
    {
        if (_disposed) return;
        try
        {
            TryRecover(reason);
        }
        catch (Exception ex)
        {
            DiagnosticLog.Warn($"EC activation check ({reason}) failed: {ex.Message}");
        }
    }

    /// <summary>
    /// If EC activation (0x741) was lost, re-run the EC init sequence.
    /// Returns true when a recovery was performed and activation is back
    /// (WriteGuard uses this to decide whether retrying a failed write is
    /// worthwhile); false when activation was never lost. Thread-safe —
    /// concurrent callers serialize, and a caller that waited out someone
    /// else's just-finished recovery still gets true so it retries its write.
    /// </summary>
    public bool TryRecover(string reason)
    {
        lock (_recoveryLock)
        {
            if (_disposed) return false;

            if (_activation.IsUnlocked())
            {
                // Healthy — or another caller recovered moments ago while we
                // waited on the lock. Report the recent recovery so a failed
                // write still gets its retry.
                return (DateTime.UtcNow - _lastRecoveryUtc) < TimeSpan.FromSeconds(10);
            }

            DiagnosticLog.Warn($"EC activation lost (0x741 bit 0 clear, detected via {reason}) — re-running EC init");

            List<string> errors;
            using (WriteGuard.EnterRecoveryScope())
                errors = _activation.InitializeEc();

            bool restored = _activation.IsUnlocked();
            if (restored)
            {
                _lastRecoveryUtc = DateTime.UtcNow;
                DiagnosticLog.Write(errors.Count == 0
                    ? "EC re-activation succeeded"
                    : $"EC re-activation succeeded with {errors.Count} init error(s)");
                Reactivated?.Invoke();
            }
            else
            {
                DiagnosticLog.Error("EC re-activation FAILED — 0x741 bit 0 still clear, EC is ignoring writes");
            }
            return restored;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        SystemEvents.PowerModeChanged -= OnPowerModeChanged;

        // Wait briefly for an in-flight tick so recovery (~4.2s worst case)
        // can't touch the EC device mid-shutdown.
        if (_timer != null)
        {
            using var done = new ManualResetEvent(false);
            if (_timer.Dispose(done))
                done.WaitOne(5000);
        }
    }
}
