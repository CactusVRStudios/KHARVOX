using System.Diagnostics;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;

namespace KharvoxLauncher;

internal enum BhapticsPreflightState
{
    Disabled,
    Ready,
    MissingBridge,
    MissingSdk
}

internal readonly struct BhapticsPreflight
{
    public BhapticsPreflightState State { get; }
    public bool CanStart => State == BhapticsPreflightState.Ready;
    public string Status { get; }

    public BhapticsPreflight(BhapticsPreflightState state, string status)
    {
        State = state;
        Status = status;
    }
}

internal sealed class BhapticsBridgeSession
{
    private const uint ProtocolMagic = 0x3242484b;
    private const ushort ProtocolVersion = 1;
    private const ushort HelloMessage = 1;
    private const ushort ShutdownMessage = 4;
    private readonly Process process;
    private readonly string pipeName;
    private readonly bool playerWasAlreadyRunning;
    private string sessionToken;
    private int stopping;

    private BhapticsBridgeSession(Process process, string pipeName, string sessionToken,
        bool playerWasAlreadyRunning)
    {
        this.process = process;
        this.pipeName = pipeName;
        this.sessionToken = sessionToken;
        this.playerWasAlreadyRunning = playerWasAlreadyRunning;
    }

    internal static string SharedSdkDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "KHARVOX", "bhaptics");

    internal static string? FindSdkDirectory(string runtimeDirectory, string? sharedDirectory)
    {
        if (File.Exists(Path.Combine(runtimeDirectory, "bhaptics_library.dll")))
            return Path.GetFullPath(runtimeDirectory);
        if (!string.IsNullOrWhiteSpace(sharedDirectory)
            && File.Exists(Path.Combine(sharedDirectory, "bhaptics_library.dll")))
            return Path.GetFullPath(sharedDirectory!);
        return null;
    }

    internal static BhapticsPreflight Inspect(bool enabled, string runtimeDirectory,
        string? sharedDirectory = null)
    {
        if (!enabled)
            return new BhapticsPreflight(BhapticsPreflightState.Disabled,
                "bHaptics disabled.");
        if (!File.Exists(Path.Combine(runtimeDirectory, "KharvoxBhapticsBridge.exe")))
            return new BhapticsPreflight(BhapticsPreflightState.MissingBridge,
                "bHaptics bridge missing; continuing without bHaptics.");
        if (FindSdkDirectory(runtimeDirectory, sharedDirectory) is null)
            return new BhapticsPreflight(BhapticsPreflightState.MissingSdk,
                "bHaptics SDK DLL missing; continuing without bHaptics.");
        return new BhapticsPreflight(BhapticsPreflightState.Ready,
            "bHaptics bridge starting …");
    }

    internal static BhapticsBridgeSession? TryStart(
        bool enabled, string runtimeDirectory, Action<string>? statusUpdate,
        string? appId = null, string? apiKey = null, bool extendedLogging = false)
    {
        var preflight = Inspect(enabled, runtimeDirectory, SharedSdkDirectory);
        if (!preflight.CanStart)
        {
            if (enabled) statusUpdate?.Invoke(preflight.Status);
            return null;
        }

        appId ??= Environment.GetEnvironmentVariable("KHARVOX_BHAPTICS_APP_ID");
        apiKey ??= Environment.GetEnvironmentVariable("KHARVOX_BHAPTICS_API_KEY");
        // Normal users need no Portal access for KHARVOX's dynamic playDot
        // mapping. A complete pair remains an optional private developer
        // override for future named Portal-event work. Never use half a pair.
        if (string.IsNullOrWhiteSpace(appId) != string.IsNullOrWhiteSpace(apiKey))
        {
            statusUpdate?.Invoke(
                "Incomplete private bHaptics developer access ignored; using local playback.");
            appId = null;
            apiKey = null;
        }

        try
        {
            // The bHaptics Player is comparatively expensive to cold-start. Remember
            // whether the Bridge has to launch it so DOOM/VDXR can be kept out of the
            // Player's WebView/Bluetooth initialization window.
            var playerWasAlreadyRunning = IsBhapticsPlayerRunning();
            var pipeName = "KharvoxBhaptics_" + Process.GetCurrentProcess().Id + "_" +
                Guid.NewGuid().ToString("N");
            var tokenBytes = new byte[32];
            using (var random = RandomNumberGenerator.Create()) random.GetBytes(tokenBytes);
            var sessionToken = BitConverter.ToString(tokenBytes).Replace("-", string.Empty);

            var startInfo = new ProcessStartInfo(
                Path.Combine(runtimeDirectory, "KharvoxBhapticsBridge.exe"))
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = runtimeDirectory
            };
            startInfo.EnvironmentVariables["KHARVOX_EXTENDED_LOGGING"] = extendedLogging ? "1" : "0";
            startInfo.EnvironmentVariables["KHARVOX_BHAPTICS_SDK_DIRECTORY"] =
                FindSdkDirectory(runtimeDirectory, SharedSdkDirectory) ?? string.Empty;
            startInfo.EnvironmentVariables["KHARVOX_BHAPTICS_PIPE_NAME"] = pipeName;
            startInfo.EnvironmentVariables["KHARVOX_BHAPTICS_SESSION_TOKEN"] = sessionToken;
            startInfo.EnvironmentVariables["KHARVOX_BHAPTICS_PARENT_PID"] =
                Process.GetCurrentProcess().Id.ToString(System.Globalization.CultureInfo.InvariantCulture);
            // Empty values select credential-free local Dot playback. Optional
            // developer values exist only in the dedicated Bridge process;
            // they are never persisted, logged, or inherited by the game.
            startInfo.EnvironmentVariables["KHARVOX_BHAPTICS_APP_ID"] = appId ?? string.Empty;
            startInfo.EnvironmentVariables["KHARVOX_BHAPTICS_API_KEY"] = apiKey ?? string.Empty;
            var process = Process.Start(startInfo);
            if (process is null)
            {
                statusUpdate?.Invoke("bHaptics bridge did not start; continuing without bHaptics.");
                return null;
            }
            statusUpdate?.Invoke(preflight.Status);
            return new BhapticsBridgeSession(process, pipeName, sessionToken,
                playerWasAlreadyRunning);
        }
        catch
        {
            statusUpdate?.Invoke("bHaptics bridge failed; continuing without bHaptics.");
            return null;
        }
    }

    // The Bridge creates its secured pipe only after its bounded SDK/Player
    // initialization attempt has finished. An authenticated probe therefore gives
    // the launcher a readiness barrier without loading the SDK here and without
    // waiting for a vest. The game reconnects to a fresh pipe instance afterwards.
    internal async Task<bool> WaitForStartupAsync(Action<string>? statusUpdate)
    {
        // initializeBackend has two independently bounded polling phases (Player
        // process and local websocket). Keep the launcher barrier longer than their
        // combined worst case, then fail open for the game start.
        var deadline = DateTime.UtcNow.AddSeconds(22);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                if (process.HasExited)
                {
                    statusUpdate?.Invoke(
                        "bHaptics bridge ended during startup; continuing without bHaptics.");
                    return false;
                }

                using var pipe = new NamedPipeClientStream(".", pipeName,
                    PipeDirection.Out, PipeOptions.Asynchronous);
                await Task.Run(() => pipe.Connect(250));
                using (var writer = new BinaryWriter(pipe, Encoding.ASCII, true))
                {
                    WriteHello(writer, 1);
                    writer.Flush();
                }

                var playerIsRunning = IsBhapticsPlayerRunning();
                var settleDelay = StartupSettleDelay(
                    playerWasAlreadyRunning, playerIsRunning);
                if (settleDelay > TimeSpan.Zero)
                {
                    statusUpdate?.Invoke(
                        "bHaptics Player started; stabilizing VR startup …");
                    await Task.Delay(settleDelay);
                }
                else
                {
                    // Give the Bridge time to recycle the probe connection into the
                    // game-facing pipe instance. The in-game client also retries.
                    await Task.Delay(150);
                }
                statusUpdate?.Invoke("bHaptics startup finished.");
                return true;
            }
            catch (TimeoutException) { }
            catch (IOException) { }
            catch (InvalidOperationException)
            {
                statusUpdate?.Invoke(
                    "bHaptics bridge became unavailable; continuing without bHaptics.");
                return false;
            }
            catch (UnauthorizedAccessException)
            {
                statusUpdate?.Invoke(
                    "bHaptics startup IPC was unavailable; continuing without bHaptics.");
                return false;
            }
            catch (System.ComponentModel.Win32Exception)
            {
                statusUpdate?.Invoke(
                    "bHaptics startup IPC failed; continuing without bHaptics.");
                return false;
            }
            await Task.Delay(100);
        }

        statusUpdate?.Invoke(
            "bHaptics startup timed out; continuing without delaying DOOM further.");
        return false;
    }

    internal static TimeSpan StartupSettleDelay(
        bool playerWasAlreadyRunning, bool playerIsRunning)
    {
        return !playerWasAlreadyRunning && playerIsRunning
            ? TimeSpan.FromSeconds(2)
            : TimeSpan.Zero;
    }

    private static bool IsBhapticsPlayerRunning()
    {
        Process[] players;
        try
        {
            players = Process.GetProcessesByName("BhapticsPlayer");
        }
        catch
        {
            return false;
        }

        var running = false;
        foreach (var player in players)
        {
            try
            {
                if (!player.HasExited) running = true;
            }
            catch { }
            finally { player.Dispose(); }
        }
        return running;
    }

    internal static void ApplyToGame(
        ProcessStartInfo startInfo, BhapticsBridgeSession? session)
    {
        if (session is null) return;
        startInfo.EnvironmentVariables["KHARVOX_BHAPTICS_PIPE_NAME"] = session.pipeName;
        startInfo.EnvironmentVariables["KHARVOX_BHAPTICS_SESSION_TOKEN"] = session.sessionToken;
    }

    internal async Task StopAsync()
    {
        if (Interlocked.Exchange(ref stopping, 1) != 0)
            return;
        try
        {
            if (process.HasExited) return;
            await Task.Run(SendShutdown).ConfigureAwait(false);
            for (var i = 0; i < 30 && !process.HasExited; ++i)
                await Task.Delay(100).ConfigureAwait(false);
            // This is an exceptional fallback only. Normal shutdown is the
            // authenticated Shutdown IPC message above.
            if (!process.HasExited)
                process.Kill();
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
