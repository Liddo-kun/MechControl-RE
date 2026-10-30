// mcctl: command-line client for a running MechControl (see architecture.md, section 12).
// Commands run inside MechControl, which owns the EC and NVIDIA state; this only forwards
// the arguments over a local pipe and prints the answer. Built by build-release.ps1 with the
// .NET Framework compiler (C# 5), so it needs no .NET 8 runtime. Keep the wire format in
// sync with MechControl/Cli/CliServer.cs.
using System;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Threading;

static class Mcctl
{
    const string PipeName = "MechControl";
    const int Version = 1;
    const int UsageError = 2, NotReachable = 3;
    const int ConnectTimeoutMs = 3000, ResponseTimeoutMs = 60000;

    const string Usage =
@"mcctl - control a running MechControl from the command line (elevated shell required)

  mcctl status [--json]
      Current settings with the ranges this machine accepts, NVIDIA limits and live
      currents, fan speeds and temperatures, thermal failsafe state.
  mcctl watch [--interval ms] [--json]
      Repeat status every interval (default 1000, minimum 250) until Ctrl+C.
      With --json, one JSON object per line.
  mcctl gpu [--tgp W] [--db W|on|off] [--nvvdd A] [--msvdd A] [--board-input A] [--json]
      GPU TGP, Dynamic Boost and NVIDIA current limits. Currents are amps (240, 10.25)
      or milliamps with an mA suffix (10250mA). Current limits need the NVIDIA unlock
      to have been turned on in the Power tab.
  mcctl cpu [--pl1 W] [--pl2 W] [--pl4 W] [--temp-limit C] [--json]
  mcctl fan boost on|off [--json]
      Maximum fan speed.

Changes apply immediately and are not saved: loading a profile, waking from sleep or an
EC re-activation puts the saved profile back. Options you leave out keep their current
values. Values outside this machine's ranges, or that would need a GPU restart or a
change of the NVIDIA unlock, are refused; so is everything except fan boost on while the
thermal failsafe is tripped.

Exit codes: 0 done, 1 refused or failed, 2 usage error, 3 MechControl not reachable.
";

    static int Main(string[] args)
    {
        if (args.Length == 0 || args[0] == "help" || args[0] == "--help" || args[0] == "-h" || args[0] == "/?")
        {
            Console.Out.Write(Usage);
            return args.Length == 0 ? UsageError : 0;
        }
        if (args[0] == "watch") return Watch(args);
        string output;
        int code = Send(args, out output);
        Print(code, output);
        return code;
    }

    static int Watch(string[] args)
    {
        int interval = 1000;
        bool json = false;
        for (int i = 1; i < args.Length; i++)
        {
            if (args[i] == "--json") json = true;
            else if (args[i] == "--interval" && i + 1 < args.Length && int.TryParse(args[i + 1], out interval) && interval >= 250) i++;
            else
            {
                Console.Error.WriteLine("Usage: mcctl watch [--interval ms] [--json]   (interval 250 ms or more)");
                return UsageError;
            }
        }
        string[] status = json ? new[] { "status", "--json" } : new[] { "status" };
        while (true)
        {
            string output;
            int code = Send(status, out output);
            if (code != 0)
            {
                Print(code, output);
                return code;
            }
            if (!json) Console.Out.WriteLine(DateTime.Now.ToString("HH:mm:ss"));
            Console.Out.WriteLine(output);
            if (!json) Console.Out.WriteLine();
            Console.Out.Flush();
            Thread.Sleep(interval);
        }
    }

    static void Print(int code, string output)
    {
        if (output.Length == 0) return;
        if (code == 0) Console.Out.WriteLine(output);
        else Console.Error.WriteLine(output);
    }

    // The exchange runs on a worker thread so a stuck MechControl can't hang the caller.
    static int Send(string[] args, out string output)
    {
        int code = NotReachable;
        string text = "MechControl did not answer within " + ResponseTimeoutMs / 1000 + " s.";
        var worker = new Thread(() =>
        {
            try { code = Exchange(args, out text); }
            catch (TimeoutException) { text = "MechControl is not running (or not responding)."; }
            catch (UnauthorizedAccessException) { text = "Access denied: run mcctl from an elevated (administrator) shell."; }
            catch (IOException ex) { text = "Lost the connection to MechControl: " + ex.Message; }
        });
        worker.IsBackground = true;
        worker.Start();
        if (!worker.Join(ResponseTimeoutMs)) code = NotReachable;
        output = text;
        return code;
    }

    static int Exchange(string[] args, out string output)
    {
        using (var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut))
        {
            pipe.Connect(ConnectTimeoutMs);
            // One write for the whole request.
            byte[] payload = Encoding.UTF8.GetBytes(string.Join("\0", args));
            var request = new MemoryStream();
            var writer = new BinaryWriter(request);
            writer.Write(Version);
            writer.Write(payload.Length);
            writer.Write(payload);
            pipe.Write(request.ToArray(), 0, (int)request.Length);
            pipe.Flush();
            var reader = new BinaryReader(pipe);
            int code = reader.ReadInt32();
            int length = reader.ReadInt32();
            byte[] text = reader.ReadBytes(length);
            if (text.Length != length) throw new EndOfStreamException("Incomplete response.");
            output = Encoding.UTF8.GetString(text);
            return code;
        }
    }
}
