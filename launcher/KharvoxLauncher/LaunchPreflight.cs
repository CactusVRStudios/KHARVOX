using Microsoft.Win32;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;

namespace KharvoxLauncher;

internal enum ProbeBoolean
{
    False,
    True,
    Unknown
}

internal enum RegistryValueProbeState
{
    Missing,
    Present,
    Inaccessible
}

internal readonly struct RegistryValueProbe
{
    public RegistryValueProbeState State { get; }
    public string? Value { get; }

    private RegistryValueProbe(RegistryValueProbeState state, string? value)
    {
        State = state;
        Value = value;
    }

    public static RegistryValueProbe Missing() =>
        new(RegistryValueProbeState.Missing, null);

    public static RegistryValueProbe Present(string? value) =>
        new(RegistryValueProbeState.Present, value);

    public static RegistryValueProbe Inaccessible() =>
        new(RegistryValueProbeState.Inaccessible, null);
}

internal readonly struct DoomRunAsAdminProbe
{
    public ProbeBoolean State { get; }
    public string? RegistryLocation { get; }

    public DoomRunAsAdminProbe(ProbeBoolean state, string? registryLocation)
    {
        State = state;
        RegistryLocation = registryLocation;
    }
}

internal sealed class LaunchPreflightResult
{
    public ProbeBoolean LauncherElevation { get; }
    public DoomRunAsAdminProbe DoomRunAsAdmin { get; }
    public ProbeBoolean SteamElevation { get; }
    public bool Passed { get; }
    public string FailureReason { get; }
    public string? FailureMessage { get; }

    public bool HasProvenElevationEvidence =>
        LauncherElevation == ProbeBoolean.True
        || DoomRunAsAdmin.State == ProbeBoolean.True
        || SteamElevation == ProbeBoolean.True;

    internal LaunchPreflightResult(ProbeBoolean launcherElevation,
        DoomRunAsAdminProbe doomRunAsAdmin, ProbeBoolean steamElevation,
        bool passed, string failureReason, string? failureMessage)
    {
        LauncherElevation = launcherElevation;
        DoomRunAsAdmin = doomRunAsAdmin;
        SteamElevation = steamElevation;
        Passed = passed;
        FailureReason = failureReason;
        FailureMessage = failureMessage;
    }

    public string StructuredLogEntry() =>
        "launcherElevated=" + LaunchPreflight.LogValue(LauncherElevation)
        + " doomRunAsAdminCompatibility=" + LaunchPreflight.LogValue(DoomRunAsAdmin.State)
        + " doomRunAsAdminRegistryLocation="
        + (string.IsNullOrWhiteSpace(DoomRunAsAdmin.RegistryLocation)
            ? "<none>" : Quote(DoomRunAsAdmin.RegistryLocation!))
        + " steamElevation=" + LaunchPreflight.LogValue(SteamElevation)
        + " preflightResult=" + (Passed ? "passed" : "failed")
        + " preflightFailureReason=" + FailureReason;

    private static string Quote(string value) =>
        "\"" + value.Replace("\"", "'") + "\"";
}

internal static class LaunchPreflight
{
    internal const string AppCompatLayersPath =
        @"Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers";

    internal delegate RegistryValueProbe RegistryValueReader(
        RegistryHive hive, RegistryView view, string subKeyPath, string valueName);

    private static readonly Regex RunAsAdminToken = new(
        @"(?<![A-Z0-9_])RUNASADMIN(?![A-Z0-9_])",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    private const uint TokenQuery = 0x0008;
    private const int TokenElevationClass = 20;

    [StructLayout(LayoutKind.Sequential)]
    private struct TokenElevation
    {
        public int TokenIsElevated;
    }

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool OpenProcessToken(IntPtr processHandle,
        uint desiredAccess, out IntPtr tokenHandle);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool GetTokenInformation(IntPtr tokenHandle,
        int tokenInformationClass, out TokenElevation tokenInformation,
        int tokenInformationLength, out int returnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    public static LaunchPreflightResult Inspect(string gameExe)
    {
        var launcherElevation = GetCurrentProcessElevation();
        var doomRunAsAdmin = ProbeDoomRunAsAdmin(gameExe, ReadRegistryValue);
        var steamElevation = ProbeRunningSteamElevation();
        return Evaluate(launcherElevation, doomRunAsAdmin, steamElevation);
    }

    internal static LaunchPreflightResult Evaluate(ProbeBoolean launcherElevation,
        DoomRunAsAdminProbe doomRunAsAdmin, ProbeBoolean steamElevation)
    {
        if (launcherElevation == ProbeBoolean.True)
            return new LaunchPreflightResult(launcherElevation, doomRunAsAdmin,
                steamElevation, false, "launcher-elevated",
                "KHARVOX is running as administrator. The Windows Vulkan loader ignores " +
                "the user-specific KHARVOX layer registration (HKCU) for elevated processes. " +
                "Close KHARVOX and start KHARVOX, Steam, and DOOM normally, without " +
                "'Run as administrator'.");

        if (launcherElevation == ProbeBoolean.Unknown)
            return new LaunchPreflightResult(launcherElevation, doomRunAsAdmin,
                steamElevation, false, "launcher-elevation-unknown",
                "KHARVOX could not determine whether the launcher is running as administrator. " +
                "For a reliable Vulkan-layer launch, start KHARVOX normally and make sure " +
                "'Run as administrator' is disabled for KHARVOX, Steam, and DOOM.");

        if (doomRunAsAdmin.State == ProbeBoolean.True)
            return new LaunchPreflightResult(launcherElevation, doomRunAsAdmin,
                steamElevation, false, "doom-runasadmin-compatibility",
                "DOOMx64vk.exe is configured to run as administrator in Windows compatibility " +
                "settings at " + doomRunAsAdmin.RegistryLocation + ". Disable 'Run this program " +
                "as an administrator' for DOOMx64vk.exe. KHARVOX did not change or remove the " +
                "registry entry.");

        if (steamElevation == ProbeBoolean.True)
            return new LaunchPreflightResult(launcherElevation, doomRunAsAdmin,
                steamElevation, false, "steam-elevated",
                "Steam is running as administrator. The Windows Vulkan loader may then ignore " +
                "the user-specific KHARVOX layer registration (HKCU). Exit Steam completely and " +
                "start Steam and KHARVOX normally, without 'Run as administrator'.");

        return new LaunchPreflightResult(launcherElevation, doomRunAsAdmin,
            steamElevation, true, "none", null);
    }

    internal static DoomRunAsAdminProbe ProbeDoomRunAsAdmin(string gameExe,
        RegistryValueReader reader)
    {
        var inaccessible = false;
        string? firstInaccessibleLocation = null;
        foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
        foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
        {
            var location = RegistryLocation(hive, view);
            RegistryValueProbe probe;
            try
            {
                probe = reader(hive, view, AppCompatLayersPath, gameExe);
            }
            catch
            {
                probe = RegistryValueProbe.Inaccessible();
            }

            if (probe.State == RegistryValueProbeState.Inaccessible)
            {
                inaccessible = true;
                firstInaccessibleLocation ??= location;
                continue;
            }

            if (probe.State == RegistryValueProbeState.Present
                && ContainsRunAsAdmin(probe.Value))
                return new DoomRunAsAdminProbe(ProbeBoolean.True, location);
        }

        return new DoomRunAsAdminProbe(
            inaccessible ? ProbeBoolean.Unknown : ProbeBoolean.False,
            firstInaccessibleLocation);
    }

    internal static bool ContainsRunAsAdmin(string? compatibilityValue) =>
        !string.IsNullOrWhiteSpace(compatibilityValue)
        && RunAsAdminToken.IsMatch(compatibilityValue);

    internal static ProbeBoolean AggregateSteamElevations(
        IEnumerable<ProbeBoolean> elevations)
    {
        var unknown = false;
        foreach (var elevation in elevations)
        {
            if (elevation == ProbeBoolean.True) return ProbeBoolean.True;
            unknown |= elevation == ProbeBoolean.Unknown;
        }
        return unknown ? ProbeBoolean.Unknown : ProbeBoolean.False;
    }

    internal static ProbeBoolean GetCurrentProcessElevation()
    {
        try
        {
            using var process = Process.GetCurrentProcess();
            var tokenResult = TryGetProcessElevation(process);
            if (tokenResult != ProbeBoolean.Unknown) return tokenResult;
        }
        catch { }

        // This is a fallback only for the current process. It does not require
        // opening another process token and preserves UAC's effective-token
        // semantics for a normally started administrator account.
        try
        {
            using var identity = System.Security.Principal.WindowsIdentity.GetCurrent();
            return new System.Security.Principal.WindowsPrincipal(identity).IsInRole(
                System.Security.Principal.WindowsBuiltInRole.Administrator)
                ? ProbeBoolean.True : ProbeBoolean.False;
        }
        catch { return ProbeBoolean.Unknown; }
    }

    internal static string LogValue(ProbeBoolean value) => value switch
    {
        ProbeBoolean.True => "true",
        ProbeBoolean.False => "false",
        _ => "unknown"
    };

    private static ProbeBoolean ProbeRunningSteamElevation()
    {
        Process[] processes;
        try { processes = Process.GetProcessesByName("steam"); }
        catch { return ProbeBoolean.Unknown; }

        var results = new List<ProbeBoolean>();
        foreach (var process in processes)
        {
            using (process)
                results.Add(TryGetProcessElevation(process));
        }
        return AggregateSteamElevations(results);
    }

    private static ProbeBoolean TryGetProcessElevation(Process process)
    {
        IntPtr token = IntPtr.Zero;
        try
        {
            if (!OpenProcessToken(process.Handle, TokenQuery, out token))
                return ProbeBoolean.Unknown;
            var size = Marshal.SizeOf(typeof(TokenElevation));
            if (!GetTokenInformation(token, TokenElevationClass,
                    out var elevation, size, out _))
                return ProbeBoolean.Unknown;
            return elevation.TokenIsElevated != 0
                ? ProbeBoolean.True : ProbeBoolean.False;
        }
        catch
        {
            return ProbeBoolean.Unknown;
        }
        finally
        {
            if (token != IntPtr.Zero) CloseHandle(token);
        }
    }

    private static RegistryValueProbe ReadRegistryValue(RegistryHive hive,
        RegistryView view, string subKeyPath, string valueName)
    {
        try
        {
            using var baseKey = RegistryKey.OpenBaseKey(hive, view);
            using var key = baseKey.OpenSubKey(subKeyPath, false);
            if (key is null) return RegistryValueProbe.Missing();
            var value = key.GetValue(valueName, null,
                RegistryValueOptions.DoNotExpandEnvironmentNames);
            return value is null
                ? RegistryValueProbe.Missing()
                : RegistryValueProbe.Present(Convert.ToString(value));
        }
        catch (UnauthorizedAccessException)
        {
            return RegistryValueProbe.Inaccessible();
        }
        catch (System.Security.SecurityException)
        {
            return RegistryValueProbe.Inaccessible();
        }
        catch
        {
            return RegistryValueProbe.Inaccessible();
        }
    }

    private static string RegistryLocation(RegistryHive hive, RegistryView view) =>
        (hive == RegistryHive.CurrentUser ? "HKCU" : "HKLM")
        + " " + view + "\\" + AppCompatLayersPath;
}

internal static class VulkanLoaderStartupDiagnosis
{
    internal static bool ShowsKharvoxLayer(string loaderLog) =>
        loaderLog.IndexOf("KharvoxLayer.json", StringComparison.OrdinalIgnoreCase) >= 0
        || loaderLog.IndexOf("VK_LAYER_KHARVOX_OPENXR", StringComparison.OrdinalIgnoreCase) >= 0
        || loaderLog.IndexOf("KharvoxLayer.dll", StringComparison.OrdinalIgnoreCase) >= 0;

    internal static string AddToFailureMessage(string baseMessage,
        string capturedLoaderLog, bool elevationProven)
    {
        if (ShowsKharvoxLayer(capturedLoaderLog)) return baseMessage;

        var cause = elevationProven
            ? " The launch preflight confirmed elevated process rights as the cause."
            : " Elevated process rights are one possible cause; make sure KHARVOX, Steam, " +
              "and DOOM are not started with 'Run as administrator'.";
        return baseMessage
            + " The captured Vulkan-loader log shows that the KHARVOX layer was not loaded."
            + cause;
    }
}
