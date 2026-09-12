using System.Diagnostics;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;

namespace KharvoxLauncher;

internal enum Psvr2PreflightState
{
    Disabled,
    Ready,
    MissingBridge,
    MissingLoader
}

internal readonly struct Psvr2Preflight
{
    public Psvr2PreflightState State { get; }
    public bool CanStart => State == Psvr2PreflightState.Ready;
    public string Status { get; }

    public Psvr2Preflight(Psvr2PreflightState state, string status)
    {
        State = state;
        Status = status;
    }
}

internal sealed class Psvr2BridgeSession
{
    private const uint ProtocolMagic = 0x3250484b;
    private const ushort ProtocolVersion = 1;
    private const ushort HelloMessage = 1;
    private const ushort ShutdownMessage = 3;
    private readonly Process process;
    private readonly string pipeName;
    private string sessionToken;
    private int stopping;

    private Psvr2BridgeSession(Process process, string pipeName, string sessionToken)
    {
        this.process = process;
        this.pipeName = pipeName;
        this.sessionToken = sessionToken;
    }

    internal static Psvr2Preflight Inspect(bool enabled, string runtimeDirectory)
    {
        if (!enabled)
            return new Psvr2Preflight(Psvr2PreflightState.Disabled,
                "PSVR2 Toolkit disabled.");
        if (!File.Exists(Path.Combine(runtimeDirectory, "KharvoxPsvr2Bridge.exe")))
            return new Psvr2Preflight(Psvr2PreflightState.MissingBridge,
                "PSVR2 Toolkit bridge missing; continuing without adaptive triggers.");
        if (!File.Exists(Path.Combine(runtimeDirectory,
                "psvr2_toolkit_capi_loader.dll")))
            return new Psvr2Preflight(Psvr2PreflightState.MissingLoader,
                "PSVR2 Toolkit loader missing; continuing without adaptive triggers.");
        return new Psvr2Preflight(Psvr2PreflightState.Ready,
            "PSVR2 Toolkit bridge starting …");
    }

    internal static Psvr2BridgeSession? TryStart(bool enabled,
        string runtimeDirectory, Action<string>? statusUpdate, bool extendedLogging = false)
    {
        var preflight = Inspect(enabled, runtimeDirectory);
        if (!preflight.CanStart)
        {
            if (enabled) statusUpdate?.Invoke(preflight.Status);
            return null;
        }
        try
        {
            var pipeName = "KharvoxPsvr2_" + Process.GetCurrentProcess().Id + "_" +
                Guid.NewGuid().ToString("N");
            var tokenBytes = new byte[32];
            using (var random = RandomNumberGenerator.Create()) random.GetBytes(tokenBytes);
            var sessionToken = BitConverter.ToString(tokenBytes).Replace("-", string.Empty);

            var startInfo = new ProcessStartInfo(
                Path.Combine(runtimeDirectory, "KharvoxPsvr2Bridge.exe"))
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = runtimeDirectory
            };
            startInfo.EnvironmentVariables["KHARVOX_EXTENDED_LOGGING"] = extendedLogging ? "1" : "0";
            startInfo.EnvironmentVariables["KHARVOX_PSVR2_PIPE_NAME"] = pipeName;
            startInfo.EnvironmentVariables["KHARVOX_PSVR2_SESSION_TOKEN"] = sessionToken;
            startInfo.EnvironmentVariables["KHARVOX_PSVR2_PARENT_PID"] =
                Process.GetCurrentProcess().Id.ToString(
                    System.Globalization.CultureInfo.InvariantCulture);
            var process = Process.Start(startInfo);
            if (process is null)
            {
                statusUpdate?.Invoke(
                    "PSVR2 Toolkit bridge did not start; continuing without adaptive triggers.");
                return null;
            }
            statusUpdate?.Invoke(preflight.Status);
            return new Psvr2BridgeSession(process, pipeName, sessionToken);
        }
        catch
        {
            statusUpdate?.Invoke(
                "PSVR2 Toolkit bridge failed; continuing without adaptive triggers.");
            return null;
        }
    }

    internal static void ApplyToGame(ProcessStartInfo startInfo,
        Psvr2BridgeSession? session)
    {
        if (session is null) return;
        startInfo.EnvironmentVariables["KHARVOX_USE_PSVR2_TOOLKIT"] = "1";
        startInfo.EnvironmentVariables["KHARVOX_PSVR2_PIPE_NAME"] = session.pipeName;
        startInfo.EnvironmentVariables["KHARVOX_PSVR2_SESSION_TOKEN"] = session.sessionToken;
    }

    internal async Task StopAsync()
    {
        if (Interlocked.Exchange(ref stopping, 1) != 0) return;
        try
        {
            if (process.HasExited) return;
            await Task.Run(SendShutdown).ConfigureAwait(false);
            for (var index = 0; index < 30 && !process.HasExited; ++index)
                await Task.Delay(100).ConfigureAwait(false);
            if (!process.HasExited) process.Kill();
            await Task.Run(() => process.WaitForExit()).ConfigureAwait(false);
        }
        catch (InvalidOperationException) { }
        catch (System.ComponentModel.Win32Exception) { }
        finally
        {
            process.Dispose();
            sessionToken = string.Empty;
        }
    }

    private void SendShutdown()
    {
        var deadline = DateTime.UtcNow.AddSeconds(2);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                using var pipe = new NamedPipeClientStream(".", pipeName,
                    PipeDirection.Out, PipeOptions.Asynchronous);
                pipe.Connect(250);
                using var writer = new BinaryWriter(pipe, Encoding.ASCII, true);
                WriteHello(writer, 1);
                WriteHeader(writer, ShutdownMessage, 2, 0);
                writer.Flush();
                return;
            }
            catch (TimeoutException) { }
            catch (IOException) { }
            Thread.Sleep(50);
        }
    }

    private void WriteHello(BinaryWriter writer, uint sequence)
    {
        var token = Encoding.ASCII.GetBytes(sessionToken);
        WriteHeader(writer, HelloMessage, sequence, checked((uint)(2 + token.Length)));
        writer.Write(checked((ushort)token.Length));
        writer.Write(token);
    }

    private static void WriteHeader(BinaryWriter writer, ushort type,
        uint sequence, uint payloadBytes)
    {
        writer.Write(ProtocolMagic);
        writer.Write(ProtocolVersion);
        writer.Write(type);
        writer.Write(sequence);
        writer.Write((ulong)Stopwatch.GetTimestamp() * 1000UL /
            (ulong)Stopwatch.Frequency);
        writer.Write(payloadBytes);
    }
}
