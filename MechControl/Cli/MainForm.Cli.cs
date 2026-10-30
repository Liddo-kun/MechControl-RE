using MechControl.Cli;

namespace MechControl.UI;

// mcctl wiring for MainForm. Compiled only with -p:EnableCli=true (build-release.ps1 -WithCli).
partial class MainForm
{
    private CliServer? _cliServer;

    // Called from Load: commands run on the UI thread, so the message loop must be running.
    partial void StartCli()
    {
        var cli = new CliCommands(_profile.ProjectId, _cpuControl, _gpuControl, _fanControl, _gpuFailsafe, _monitor.ReadNow);
        cli.PowerApplied += () => _powerTab?.RefreshFromEc();
        cli.FanApplied += () => _fanTab?.RefreshFromEc();
        // Commands queued while a modal dialog pumps messages (e.g. a GPU restart's progress
        // notice) would run in the middle of that operation: refuse them instead.
        _cliServer = new CliServer(args => Application.OpenForms.Cast<Form>().Any(f => f.Modal)
            ? CliResult.Error("MechControl is showing a dialog (for example during a GPU restart). Try again when it closes.")
            : cli.Execute(args), SynchronizationContext.Current!);
        _cliServer.Start();
    }

    partial void StopCli() => _cliServer?.Dispose();
}
