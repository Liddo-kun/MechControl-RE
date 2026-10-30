using System.IO.Pipes;
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;

namespace MechControl.Cli;

/// <summary>
/// Local named pipe for mcctl.exe. Only elevated administrators and SYSTEM may connect:
/// anyone who can write to it can change GPU power and current limits. One request at a
/// time, executed on the UI thread through <c>ui</c>.
/// Wire format (little-endian; keep in sync with mcctl/mcctl.cs):
///   request  = int32 version, int32 byte count, UTF-8 arguments separated by NUL
///   response = int32 exit code, int32 byte count, UTF-8 text
/// </summary>
sealed class CliServer : IDisposable
{
    public const string DefaultPipeName = "MechControl";
    public const int Version = 1;
    private const int MaxRequestBytes = 64 * 1024;
    // Covers reading a request and the client reading the response, not running the command.
    private static readonly TimeSpan IoTimeout = TimeSpan.FromSeconds(5);

    private readonly Func<string[], CliResult> _execute;
    private readonly SynchronizationContext _ui;
    private readonly string _pipeName;
    private readonly CancellationTokenSource _stop = new();
    private Task? _loop;

    public CliServer(Func<string[], CliResult> execute, SynchronizationContext ui, string pipeName = DefaultPipeName)
    {
        _execute = execute;
        _ui = ui;
        _pipeName = pipeName;
    }

    public void Start() => _loop ??= Task.Run(RunAsync);

    public void Dispose() => _stop.Cancel();

    private async Task RunAsync()
    {
        string lastError = "";
        while (!_stop.IsCancellationRequested)
        {
            try
            {
                using var pipe = Create();
                DiagnosticLog.Write($@"[CLI] Listening on \\.\pipe\{_pipeName} (administrators only).");
                lastError = "";
                while (true)
                {
                    await pipe.WaitForConnectionAsync(_stop.Token);
                    try { await ServeAsync(pipe); }
                    catch (Exception ex) when (!_stop.IsCancellationRequested) { DiagnosticLog.Warn($"[CLI] Request failed: {ex.Message}"); }
                    pipe.Disconnect();
                }
            }
            catch (OperationCanceledException) when (_stop.IsCancellationRequested) { return; }
            catch (Exception ex)
            {
                // E.g. another process holds the pipe name. Keep retrying; log each new reason once.
                if (ex.Message != lastError) DiagnosticLog.Warn($"[CLI] Command-line pipe unavailable: {ex.Message} Retrying.");
                lastError = ex.Message;
                try { await Task.Delay(5000, _stop.Token); }
                catch (OperationCanceledException) { return; }
            }
        }
    }

    private NamedPipeServerStream Create()
    {
        var security = new PipeSecurity();
        foreach (var sid in new[] { WellKnownSidType.BuiltinAdministratorsSid, WellKnownSidType.LocalSystemSid })
            security.AddAccessRule(new PipeAccessRule(new SecurityIdentifier(sid, null), PipeAccessRights.FullControl, AccessControlType.Allow));
        // FirstPipeInstance: fail instead of joining a pipe someone else created under this name.
        return NamedPipeServerStreamAcl.Create(_pipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
            PipeOptions.Asynchronous | PipeOptions.FirstPipeInstance, 0, 0, security);
    }

    private async Task ServeAsync(NamedPipeServerStream pipe)
    {
        CliResult result;
        using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token))
        {
            timeout.CancelAfter(IoTimeout);
            var header = new byte[8];
            await pipe.ReadExactlyAsync(header, timeout.Token);
            int version = BitConverter.ToInt32(header, 0), length = BitConverter.ToInt32(header, 4);
            if (length is < 0 or > MaxRequestBytes) throw new InvalidDataException("Invalid request size.");
            // Read the whole request even from another version, so the answer isn't lost to a
            // client still writing. Future versions must keep this two-int32 header.
            var payload = new byte[length];
            await pipe.ReadExactlyAsync(payload, timeout.Token);
            result = version != Version
                ? new(CliResult.UsageError, $"This mcctl speaks protocol {version}, MechControl {Version}: use the mcctl.exe next to MechControl.exe.")
                : await RunOnUiAsync(length == 0 ? [] : Encoding.UTF8.GetString(payload).Split('\0'));
        }

        byte[] text = Encoding.UTF8.GetBytes(result.Output);
        var response = new byte[8 + text.Length];
        BitConverter.TryWriteBytes(response.AsSpan(0, 4), result.ExitCode);
        BitConverter.TryWriteBytes(response.AsSpan(4, 4), text.Length);
        text.CopyTo(response, 8);
        using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(_stop.Token))
        {
            timeout.CancelAfter(IoTimeout);
            await pipe.WriteAsync(response, timeout.Token);
            // Disconnecting discards unread data: wait for the client to read and close its end.
            try { await pipe.ReadAsync(new byte[1], timeout.Token); }
            catch (OperationCanceledException) when (!_stop.IsCancellationRequested) { }
        }
    }

    private async Task<CliResult> RunOnUiAsync(string[] args)
    {
        var done = new TaskCompletionSource<CliResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        using var stopping = _stop.Token.Register(() => done.TrySetCanceled());
        try
        {
            _ui.Post(_ =>
            {
                try { done.TrySetResult(_execute(args)); }
                catch (Exception ex) { done.TrySetResult(CliResult.Error(ex.Message)); }
            }, null);
        }
        catch (Exception ex) { done.TrySetResult(CliResult.Error("MechControl is shutting down: " + ex.Message)); }
        return await done.Task;
    }
}
