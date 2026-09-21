using System.Diagnostics;
using System.Threading;

namespace KharvoxLauncher;

// The owned helper must exit before DOOM can acquire the runtime and GPU.
internal sealed class VrGameIntroSession : IDisposable
{
    internal static string MarkerPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "KHARVOX", "vr-cube-seen-v2");
    internal static void Reset(string? markerPath = null) => File.Delete(markerPath ?? MarkerPath);
    // Release number, independent of Git metadata and the native DLL build counter.
    internal static Version ReleaseVersion {
        get {
            var version = typeof(VrGameIntroSession).Assembly.GetName().Version!;
            return new Version(version.Major, version.Minor, Math.Max(0, version.Build));
        }
    }
    internal static bool NeedsIntro(string? seen, Version current) =>
        !Version.TryParse(seen?.Trim(), out var previous) || current > previous;

    internal static bool ShouldPlayIntro(bool seenCurrentRelease, bool disableVrIntro) =>
        !seenCurrentRelease || !disableVrIntro;

    internal static bool HasSeenCurrentRelease => !NeedsIntroFile(MarkerPath);

    private static bool NeedsIntroFile(string path) {
        try { return NeedsIntro(File.ReadAllText(path), ReleaseVersion); }
        catch (IOException) { return true; }
        catch (UnauthorizedAccessException) { return true; }
    }
    internal string Token { get; } = Guid.NewGuid().ToString("N");
    private readonly EventWaitHandle dismissed;
    private readonly EventWaitHandle release;
    private readonly EventWaitHandle released;
    private Process? process;
    private bool disposed;

    private VrGameIntroSession()
    {
        var prefix = @"Local\KHARVOX-VR-INTRO-" + Token;
        dismissed = new EventWaitHandle(false, EventResetMode.ManualReset, prefix + "-dismissed");
        release = new EventWaitHandle(false, EventResetMode.ManualReset, prefix + "-release");
        released = new EventWaitHandle(false, EventResetMode.ManualReset, prefix + "-released");
    }

    internal static Exception StartupFailure(int exitCode) => exitCode == HeadsetUnavailableException.ExitCode
        ? new HeadsetUnavailableException()
        : new InvalidOperationException("VR GameIntro ended before dismissal. DOOM was not started. See %TEMP%/KHARVOX-VR-INTRO.log.");

    internal static async Task<VrGameIntroSession?> StartAsync(string directory, Action<string>? status, string? markerPath = null, bool disableVrIntro = false)
    {
        markerPath ??= MarkerPath;
        if (!ShouldPlayIntro(!NeedsIntroFile(markerPath), disableVrIntro)) return null;
        var executable = Path.Combine(directory, "KharvoxLauncher.exe");
        if (!File.Exists(executable) || !File.Exists(Path.Combine(directory, "KharvoxLayer.dll")))
        {
            status?.Invoke("VR intro host is missing; skipping intro and starting DOOM.");
            return null;
        }
        var intro = new VrGameIntroSession();
        try
        {
            using var launcher = Process.GetCurrentProcess();
            var info = new ProcessStartInfo(executable, "--vr-intro " + intro.Token + " " + launcher.Id)
            {
                UseShellExecute = false, CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = directory
            };
            // A graphics helper must not inherit diagnostic Vulkan layer forcing.
            info.EnvironmentVariables.Remove("VK_INSTANCE_LAYERS");
            status?.Invoke("GameIntro in VR - press any controller button to start DOOM.");
            intro.process = Process.Start(info) ?? throw new InvalidOperationException("Could not start VR GameIntro.");
            while (!intro.dismissed.WaitOne(0))
            {
                if (intro.process.HasExited)
                {
                    if (intro.dismissed.WaitOne(0)) break;
                    throw StartupFailure(intro.process.ExitCode);
                }
                await Task.Delay(50).ConfigureAwait(false);
            }
            status?.Invoke("Closing VR intro before starting DOOM ...");
            intro.release.Set();
            // Session teardown can block inside a runtime. Bound the wait and
            // terminate only our helper, then verify OS process termination.
            await Task.Run(() => intro.StopProcess()).ConfigureAwait(false);
            Directory.CreateDirectory(Path.GetDirectoryName(markerPath)!);
            if (NeedsIntroFile(markerPath))
                File.WriteAllText(markerPath, ReleaseVersion + Environment.NewLine);
            status?.Invoke("VR intro closed. Starting DOOM ...");
            return intro;
        }
        catch { intro.Dispose(); throw; }
    }

    private void StopProcess()
    {
        if (process is null) return;
        if (!process.WaitForExit(7000))
        {
            try { process.Kill(); }
            catch (InvalidOperationException) when (process.HasExited) { }
            if (!process.WaitForExit(5000))
                throw new InvalidOperationException("VR intro could not be stopped. DOOM was not started to avoid overlapping VR sessions.");
        }
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        release.Set();
        if (process is not null)
        {
            try
            {
                StopProcess();
            }
            catch (InvalidOperationException) { }
            finally { process.Dispose(); }
        }
        dismissed.Dispose(); release.Dispose(); released.Dispose();
    }
}
