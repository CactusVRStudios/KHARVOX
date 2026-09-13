using Microsoft.Win32;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Runtime.InteropServices;
using System.Management;
using System.Security.Cryptography;

namespace KharvoxLauncher;

internal sealed class KharvoxLaunchOptions
{
    public bool ImmersiveMode { get; }
    public bool CinematicFreelook { get; }
    public bool OtherCinematicsInQuad { get; }
    public bool CinewindowFollowsHeadset { get; }
    public string RendererMode { get; }
    public decimal RenderScale { get; }
    public bool UseFsrUpscaling { get; }
    public string GameDirectory { get; }
    public string TurnMode { get; }
    public string MovementDirection { get; }
    public decimal SmoothTurnSpeed { get; }
    public decimal SnapTurnAngle { get; }
    public decimal TurnDeadzone { get; }
    public bool TwoHandCalibration { get; }
    public string TwoHandCalibrationWeapon { get; }
    public string TwoHandAlignment { get; }
    public bool VirtualGunstock { get; }
    public bool PhysicalGlorykill { get; }
    public decimal PhysicalGlorykillSpeed { get; }
    public string PhysicalGlorykillHands { get; }
    public bool LeftHanded { get; }
    public string LeftHandSwapMode { get; }
    public bool LaserSight { get; }
    public bool HudDebugging { get; }
    public bool ExtendedLogging { get; }
    public bool CaptureEyes { get; }
    public bool DisableAa { get; }
    public bool HandsJump { get; }
    public bool ShowHands { get; }
    public bool DisableVrIntro { get; }
    public string HandCalibrationMode { get; }
    public bool EnableBhaptics { get; }
    public bool UsePsvr2Toolkit { get; }
    public string BackWeapon { get; }

    public KharvoxLaunchOptions(bool immersiveMode, bool cinematicFreelook,
        bool otherCinematicsInQuad, bool cinewindowFollowsHeadset,
        string rendererMode, decimal renderScale, bool useFsrUpscaling,
        string gameDirectory, string turnMode, string movementDirection,
        decimal smoothTurnSpeed,
        decimal snapTurnAngle, decimal turnDeadzone, bool twoHandCalibration,
        string twoHandCalibrationWeapon, string twoHandAlignment,
        bool virtualGunstock, bool physicalGlorykill,
        decimal physicalGlorykillSpeed, string physicalGlorykillHands,
        bool leftHanded, string leftHandSwapMode,
        bool laserSight,
        bool hudDebugging, bool extendedLogging, bool showHands,
        string handCalibrationMode,
        bool enableBhaptics,
        bool usePsvr2Toolkit,
        string backWeapon, bool handsJump = false, bool disableAa = false, bool captureEyes = false, bool disableVrIntro = false)
    {
        ImmersiveMode = immersiveMode; CinematicFreelook = cinematicFreelook;
        OtherCinematicsInQuad = otherCinematicsInQuad;
        CinewindowFollowsHeadset = cinewindowFollowsHeadset;
        RendererMode = rendererMode; RenderScale = renderScale;
        UseFsrUpscaling = useFsrUpscaling;
        GameDirectory = gameDirectory; TurnMode = turnMode;
        MovementDirection = string.Equals(movementDirection, "off-hand",
            StringComparison.OrdinalIgnoreCase) ? "off-hand" : "head";
        SmoothTurnSpeed = smoothTurnSpeed; SnapTurnAngle = snapTurnAngle; TurnDeadzone = turnDeadzone;
        TwoHandCalibration = twoHandCalibration;
        TwoHandCalibrationWeapon = twoHandCalibrationWeapon;
        TwoHandAlignment = twoHandAlignment;
        VirtualGunstock = virtualGunstock;
        PhysicalGlorykill = physicalGlorykill;
        PhysicalGlorykillSpeed = physicalGlorykillSpeed;
        PhysicalGlorykillHands = physicalGlorykillHands;
        LeftHanded = leftHanded;
        LeftHandSwapMode = leftHandSwapMode;
        LaserSight = laserSight;
        ExtendedLogging = extendedLogging;
        HandCalibrationMode = handCalibrationMode switch
        {
            "rotation" => "rotation",
            "position" => "position",
            _ => "off"
        };
        HandsJump = handsJump;
        DisableAa = disableAa;
        CaptureEyes = captureEyes;
        DisableVrIntro = disableVrIntro;

        ShowHands = showHands || HandCalibrationMode != "off";
        // Both calibration systems own Num + and the axis keys. Hand
        // calibration takes exclusive keypad ownership for this launch.
        HudDebugging = hudDebugging && HandCalibrationMode == "off";
        EnableBhaptics = enableBhaptics;
        UsePsvr2Toolkit = usePsvr2Toolkit;
        BackWeapon = NormalizeBackWeapon(backWeapon);
    }

    private static string NormalizeBackWeapon(string? key) =>
        key?.Trim().ToLowerInvariant() switch
        {
            "pistol" => "pistol",
            "shotgun" => "shotgun",
            "plasma_rifle" => "plasma_rifle",
            "heavy_assault_rifle" => "heavy_assault_rifle",
            "rocket_launcher" => "rocket_launcher",
            "super_shotgun" => "super_shotgun",
            "gauss_cannon" => "gauss_cannon",
            "chaingun" => "chaingun",
            "bfg" => "bfg",
            "chainsaw" => "chainsaw",
            _ => "shotgun"
        };

    public string DiagnosticSummary() =>
        $"gameDirectory=\"{GameDirectory}\" renderer={RendererMode} renderScale={RenderScale.ToString(CultureInfo.InvariantCulture)}% fsr1={(UseFsrUpscaling ? "enabled" : "disabled")} " +
        $"immersiveMode={(ImmersiveMode ? "enabled" : "disabled")} cinematicFreelook={(CinematicFreelook ? "enabled" : "disabled")} " +
        $"otherCinematicsInQuad={(OtherCinematicsInQuad ? "enabled" : "disabled")} " +
        $"cinewindowFollowsHeadset={(CinewindowFollowsHeadset ? "enabled" : "disabled")} " +
        $"turnMode={TurnMode} movementDirection={MovementDirection} twoHandMode={(TwoHandCalibration ? "calibration" : "gameplay-test")} " +
        $"calibrationWeapon={TwoHandCalibrationWeapon} alignment={TwoHandAlignment} virtualGunstock={(VirtualGunstock ? "enabled" : "disabled")} " +
        $"physicalGlorykill={(PhysicalGlorykill ? "enabled" : "disabled")} punchSpeed={PhysicalGlorykillSpeed.ToString(CultureInfo.InvariantCulture)}m/s punchHands={PhysicalGlorykillHands} " +
        $"handedness={(LeftHanded ? "left" : "right")} leftHandSwap={(LeftHanded ? LeftHandSwapMode : "none")} " +
        $"laserSight={(LaserSight ? "enabled" : "disabled")} " +
        $"backWeapon={BackWeapon} " +
        $"hudDebugging={(HudDebugging ? "enabled" : "disabled")} " +
        $"extendedLogging={(ExtendedLogging ? "enabled" : "disabled")} " +
        $"disableAa={DisableAa} " +
        $"handsJump={(HandsJump ? "enabled" : "disabled")} " +
        $"showHands={(ShowHands ? "enabled" : "disabled")} " +
        $"handCalibration={HandCalibrationMode} " +
        $"bHaptics={(EnableBhaptics ? "enabled" : "disabled")} " +
        $"psvr2Toolkit={(UsePsvr2Toolkit ? "enabled" : "disabled")}";
}

internal static class KharvoxRunner
{
    internal const string BuildId = "2026.09.13-launcher-v0.8-beta.4";
    private const string LayerName = "VK_LAYER_KHARVOX_OPENXR";
    private const string RegistryPath = @"SOFTWARE\Khronos\Vulkan\ImplicitLayers";
    private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;
    private static Process? currentGame;
    private static readonly object focusOperationLock = new();
    private static readonly object bhapticsLock = new();
    private static BhapticsBridgeSession? currentBhaptics;
    private static readonly object psvr2Lock = new();
    private static readonly object diagnosticLogLock = new();
    private static Psvr2BridgeSession? currentPsvr2;
    private static int launchInProgress;
    internal enum StartupState
    {
        Healthy,
        DeviceLost,
        PresentObserved,
        RuntimeNotRendering,
        PresentStreamStalled,
        Exited,
        TimedOut
    }

    private sealed class LaunchStateLease : IDisposable
    {
        private int disposed;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref disposed, 1) == 0)
                Interlocked.Exchange(ref launchInProgress, 0);
        }
    }

    public static bool IsRunning
    {
        get
        {
            try
            {
                if (currentGame is not null && !currentGame.HasExited) return true;
            }
            catch { }

            try { currentGame?.Dispose(); } catch { }
            currentGame = FindRunningDoomProcess();
            if (currentGame is not null) return true;
            if (Volatile.Read(ref launchInProgress) != 0) return false;
            StopBhapticsInBackground();
            StopPsvr2InBackground();
            return false;
        }
    }

    private static Process? FindRunningDoomProcess()
    {
        Process? found = null;
        foreach (var process in Process.GetProcessesByName("DOOMx64vk"))
        {
            try
            {
                if (found is null && !process.HasExited)
                {
                    found = process;
                    continue;
                }
            }
            catch { }
            process.Dispose();
        }
        return found;
    }

    internal static FileStream AcquireLaunchGate()
    {
        var gatePath = Path.Combine(Path.GetTempPath(), "KHARVOX-game-launch.lock");
        try
        {
            return new FileStream(gatePath, FileMode.OpenOrCreate, FileAccess.ReadWrite,
                FileShare.None, 1, FileOptions.DeleteOnClose);
        }
        catch (IOException exception)
        {
            throw new InvalidOperationException(
                "Another KHARVOX launch is already in progress. Wait for it to finish before trying again.",
                exception);
        }
    }

    private static LaunchStateLease BeginLaunch()
    {
        if (Interlocked.CompareExchange(ref launchInProgress, 1, 0) != 0)
            throw new InvalidOperationException(
                "Another KHARVOX launch is already in progress. Wait for it to finish before trying again.");
        return new LaunchStateLease();
    }

    private static void EnsureNoRunningDoom()
    {
        var runningGame = FindRunningDoomProcess();
        if (runningGame is null) return;

        try { currentGame?.Dispose(); } catch { }
        currentGame = runningGame;
        throw new InvalidOperationException(
            "DOOM is already running. End the existing game before launching KHARVOX again.");
    }

    public static async Task LaunchAsync(KharvoxLaunchOptions options, Action? gameDetected = null,
        Action<string>? statusUpdate = null, string? bhapticsAppId = null,
        string? bhapticsApiKey = null)
    {
        var runtimeDir = AppContext.BaseDirectory;
        var dllPath = Path.Combine(runtimeDir, "KharvoxLayer.dll");
        var manifestPath = Path.Combine(runtimeDir, "KharvoxLayer.json");
        var launchId = Guid.NewGuid().ToString("N").Substring(0, 12);
        var loaderLogPath = Path.Combine(Path.GetTempPath(), "KHARVOX-vulkan-loader-v0.8-beta.log");
        if (!File.Exists(dllPath)) throw new FileNotFoundException("KharvoxLayer.dll must be next to the launcher.", dllPath);

        var gameExe = Path.Combine(options.GameDirectory ?? string.Empty, "DOOMx64vk.exe");
        if (!File.Exists(gameExe)) throw new FileNotFoundException(
            "Select the DOOM (2016) installation folder containing DOOMx64vk.exe.", gameExe);

        // This must stay ahead of the launch gate (a temporary file), manifest
        // updates, controller config writes, bridge startup and Process.Start.
        // Elevated Vulkan processes do not consume the per-user HKCU layer.
        var preflight = LaunchPreflight.Inspect(gameExe);
        if (!preflight.Passed)
        {
            if (options.ExtendedLogging)
            {
                try
                {
                    AppendDiagnostic(Path.Combine(Path.GetTempPath(), "KHARVOX.log"),
                        launchId, "preflight " + preflight.StructuredLogEntry());
                }
                catch { }
            }
            throw new InvalidOperationException(preflight.FailureMessage);
        }

        if (UsesPimaxRuntime())
        {
            throw new InvalidOperationException(
                "Incompatible Runtime. Pimax OpenXR not supported. Please switch to SteamVR.");
        }

        using var launchGate = AcquireLaunchGate();
        using var launchState = BeginLaunch();
        EnsureNoRunningDoom();
        if (FileVersionInfo.GetVersionInfo(dllPath).ProductVersion != "0.8.0-beta.4")
            throw new InvalidOperationException("The 0.8 Beta launcher requires its matching 0.8 Beta KharvoxLayer.dll. Extract the complete Beta release into its own folder.");
        using var gameIntro = await VrGameIntroSession.StartAsync(runtimeDir, statusUpdate, disableVrIntro: options.DisableVrIntro).ConfigureAwait(false);
        var previousNativeFailure = NativeLaunchRecovery.Prepare(runtimeDir, options.RendererMode);
        if (previousNativeFailure is not null)
            statusUpdate?.Invoke("Retrying Native Stereo; previous failure saved in " + previousNativeFailure);
        WriteManifest(manifestPath, dllPath);
        EnsureNativeControllerBindings(options.BackWeapon);
        await StopBhapticsAsync().ConfigureAwait(false);
        await StopPsvr2Async().ConfigureAwait(false);
        var bhaptics = BhapticsBridgeSession.TryStart(
            options.EnableBhaptics, runtimeDir, statusUpdate,
            bhapticsAppId, bhapticsApiKey, options.ExtendedLogging);
        lock (bhapticsLock) currentBhaptics = bhaptics;
        var psvr2 = Psvr2BridgeSession.TryStart(
            options.UsePsvr2Toolkit, runtimeDir, statusUpdate, options.ExtendedLogging);
        lock (psvr2Lock) currentPsvr2 = psvr2;
        if (bhaptics is not null)
            await bhaptics.WaitForStartupAsync(statusUpdate);
        var launchSucceeded = false;

        var temporaryFiles = new List<string>();
        void WriteTemporary(string name, string? content = null)
        {
            var path = Path.Combine(runtimeDir, name);
            File.WriteAllText(path, content ?? string.Empty);
            temporaryFiles.Add(path);
        }

        try
        {
            var steamVrLayerIsolation = UsesSteamVrRuntime();
            var effectiveRenderScale = EffectiveRenderScale(
                options.RenderScale, steamVrLayerIsolation && !RendererSelection.IsNative(options.RendererMode));

            foreach (var flag in new[] {
                "enable_xr_mediated", "enable_xr_session", "enable_native_xr_resolution",
                "enable_6dof_weapon", "enable_two_hand_weapon",
                // r268 restores cached render poses; application turn compensation stays disabled.
                "enable_cached_eye_reprojection"
            }) WriteTemporary(flag);
            if (options.TwoHandCalibration) WriteTemporary("enable_two_hand_calibration");
            if (options.VirtualGunstock) WriteTemporary("enable_virtual_gunstock");
            WriteTemporary("two_hand_calibration_target.cfg", options.TwoHandCalibrationWeapon);
            WriteTemporary("two_hand_alignment.cfg", options.TwoHandAlignment);
            WriteTemporary("back_weapon.cfg", options.BackWeapon);
            WriteTwoHandStatus(runtimeDir, options.TwoHandCalibration
                ? "CALIBRATION: enter gameplay, equip " + options.TwoHandCalibrationWeapon +
                  ", hold both hands and press " + (options.LeftHanded ? "LEFT TRIGGER" : "RIGHT TRIGGER")
                : "GAMEPLAY TEST: each calibrated weapon uses its own " +
                  (options.LeftHanded ? "RIGHT GRIP" : "LEFT GRIP") + " support profile" +
                  (options.VirtualGunstock ? " + Virtual GunStock" : string.Empty));
            // Isolation is complete: honor the launcher renderer selection.
            // Legacy renderer selections are normalized before launch.
            var nativeStereoEnabled = RendererSelection.IsNative(options.RendererMode);
            RendererSelection.ClearObsoleteMarkers(runtimeDir);
            File.Delete(Path.Combine(runtimeDir, "fsr1_status.txt"));
            var fsr1Enabled = options.UseFsrUpscaling && options.RenderScale < 100m;
            var steamNativeSourceScale = SteamNativeResolutionScale(
                options.RenderScale, steamVrLayerIsolation && !RendererSelection.IsNative(options.RendererMode));
            var nvidiaAfwMarker = Path.Combine(runtimeDir, "enable_nvidia_afw");
            var fsr1Marker = Path.Combine(runtimeDir, "enable_fsr_upscaling");
            // A killed launcher can leave an optional marker beside the DLL.
            // Clear the exact launcher-owned selections before recreating only
            // the options selected for this run.
            foreach (var staleOptionalMarker in new[] {
                Path.Combine(runtimeDir,"enable_native_stereo_backend"),
                Path.Combine(runtimeDir,"enable_native_stereo"),
                Path.Combine(runtimeDir,"enable_native_two_view"),
                Path.Combine(runtimeDir,"enable_same_frame_stereo"),
                nvidiaAfwMarker,
                fsr1Marker,
                Path.Combine(runtimeDir, "enable_delayed_steam_source_scale"),
                Path.Combine(runtimeDir, "requested_render_scale.cfg"),
                Path.Combine(runtimeDir, "enable_immersive_cinematics_and_glory_kills"),
                Path.Combine(runtimeDir, "enable_immersive_cinematic_freelook"),
                Path.Combine(runtimeDir, "enable_other_cinematics_in_quad"),
                Path.Combine(runtimeDir, "enable_cinewindow_head_follow"),
                Path.Combine(runtimeDir, "enable_show_body"),
                Path.Combine(runtimeDir, "enable_laser_sight")
            })
            {
                try { File.Delete(staleOptionalMarker); } catch { }
            }
            if (nativeStereoEnabled) WriteTemporary("enable_native_stereo_backend");
            if (fsr1Enabled) WriteTemporary("enable_fsr_upscaling");
            if (options.LaserSight) WriteTemporary("enable_laser_sight");
            WriteTemporary("render_scale.cfg", Inv(effectiveRenderScale / 100m));
            WriteRendererStatus(runtimeDir, nativeStereoEnabled
                ? "Renderer: Native Stereo Experimental starting …"
                : fsr1Enabled ? "Renderer: FSR1 AER upscaling starting …" : "Renderer: AER starting …");
            if (options.ImmersiveMode) WriteTemporary("enable_immersive_cinematics_and_glory_kills");
            if (options.ImmersiveMode && options.CinematicFreelook)
                WriteTemporary("enable_immersive_cinematic_freelook");
            if (options.ImmersiveMode && options.OtherCinematicsInQuad)
                WriteTemporary("enable_other_cinematics_in_quad");
            if (options.CinewindowFollowsHeadset)
                WriteTemporary("enable_cinewindow_head_follow");

            WriteTemporary("weapon_scale.cfg", "0.77");
            WriteTemporary("weapon_pivot.cfg", "0 0 0");
            WriteTemporary("weapon_yaw.cfg", ReadCalibrationWithDefault(
                runtimeDir, "weapon_yaw_saved.cfg", "weapon_yaw_default.cfg", "-8"));
            WriteTemporary("weapon_roll.cfg", ReadCalibrationWithDefault(
                runtimeDir, "weapon_roll_saved.cfg", "weapon_roll_default.cfg", "-1"));
            WriteTemporary("weapon_pitch.cfg", ReadCalibrationWithDefault(
                runtimeDir, "weapon_pitch_saved.cfg", "weapon_pitch_default.cfg", "-3"));
            WriteTemporary("hands_projection_scale.cfg", "1");
            WriteTemporary("world_scale.cfg", "39.3701");

            using var userHive = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry64);
            using var key = userHive.CreateSubKey(RegistryPath, true)
                ?? throw new InvalidOperationException("The Vulkan implicit-layer registry key could not be opened.");
            DisableKharvoxLayerRegistrations(key, manifestPath);
            key.SetValue(manifestPath, 0, RegistryValueKind.DWord);
            key.Flush();

            var steamPath = FindSteam();
            var steamInstall = IsSteamInstall(options.GameDirectory ?? string.Empty);
            if (steamInstall && steamPath is null)
                throw new InvalidOperationException("Steam was not found. Start Steam once and try again.");
            var steamVrAsyncReprojection = "inactive";
            if (steamVrLayerIsolation && steamPath is not null)
            {
                try
                {
                    var update = SteamVrSettings.EnsureKharvoxAsyncReprojection(
                        SteamVrSettings.SettingsPathForSteam(steamPath));
                    steamVrAsyncReprojection = update.ToString();
                    statusUpdate?.Invoke(update == SteamVrSettingsUpdate.AlreadyConfigured
                        ? "SteamVR async reprojection is enabled for KHARVOX."
                        : "SteamVR async reprojection enabled for KHARVOX.");
                }
                catch (Exception exception)
                {
                    steamVrAsyncReprojection = "configuration-error:" + exception.GetType().Name;
                    statusUpdate?.Invoke("SteamVR async reprojection could not be configured; launching without changing SteamVR settings.");
                }
            }
            if (steamInstall && !IsProcessRunning("steam"))
            {
                Process.Start(new ProcessStartInfo(steamPath!) { UseShellExecute = false })?.Dispose();
                await Task.Delay(TimeSpan.FromSeconds(5));
            }

            // Launch the Vulkan executable directly. Sending -applaunch to an
            // already-running Steam client loses this process environment and
            // can race the temporary implicit-layer registration, producing a
            // normal flat DOOM launch with no KHARVOX.log activity.
            var psi = new ProcessStartInfo(gameExe) {
                UseShellExecute = false,
                WorkingDirectory = options.GameDirectory,
                Arguments = JoinArguments(BuildGameArguments(
                    options, false, effectiveRenderScale, nativeStereoEnabled)),
                RedirectStandardOutput = options.ExtendedLogging,
                RedirectStandardError = options.ExtendedLogging
            };
            EnableLayerForGame(psi);
            psi.EnvironmentVariables.Remove("KHARVOX_VR_INTRO");
            psi.EnvironmentVariables.Remove("KHARVOX_VR_INTRO_HANDOFF");
            if (gameIntro is not null) psi.EnvironmentVariables["KHARVOX_VR_INTRO_HANDOFF"] = gameIntro.Token;
            psi.EnvironmentVariables["KHARVOX_EXTENDED_LOGGING"] = options.ExtendedLogging ? "1" : "0";
            psi.EnvironmentVariables.Remove("VK_LOADER_DEBUG");
            if (options.ExtendedLogging)
            {
                psi.EnvironmentVariables["VK_LOADER_DEBUG"] = "error,warn,info,layer";
                psi.EnvironmentVariables["KHARVOX_EXTENDED_LOGGING"] = "1";
            }
            if (steamInstall)
            {
                psi.EnvironmentVariables["SteamAppId"] = "379720";
                psi.EnvironmentVariables["SteamGameId"] = "379720";
            }
            if (steamVrLayerIsolation)
            {
                // Keep unrelated VR/capture Vulkan layers out of Kharvox's
                // graphics chain while SteamVR is active. Steam's separately
                // injected desktop overlay is not controlled by these layer
                // switches, but no global Steam/Vulkan setting is changed.
                psi.EnvironmentVariables["DISABLE_VK_LAYER_VALVE_steam_overlay_1"] = "1";
                psi.EnvironmentVariables["DISABLE_VK_LAYER_VALVE_steam_fossilize_1"] = "1";
                psi.EnvironmentVariables["DISABLE_VULKAN_OBS_CAPTURE"] = "1";
                psi.EnvironmentVariables["EOS_OVERLAY_DISABLE_VULKAN_WIN64"] = "1";
                psi.EnvironmentVariables["SteamNoOverlayUI"] = "1";
                psi.EnvironmentVariables["SteamNoOverlayUIDrawing"] = "1";
                psi.EnvironmentVariables["DisableOverlayInjection"] = "1";

                // A Virtual Desktop Oculus-plugin compatibility API layer is
                // registered system-wide as an implicit OpenXR layer. It is
                // offered to DOOM even when SteamVR is driving a native PSVR2.
                // Use the layer manifest's own per-process disable switch so
                // this SteamVR isolation cannot alter other applications or
                // the proven VDXR runtime path.
                psi.EnvironmentVariables["DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY"] = "1";
            }
            psi.EnvironmentVariables["KHARVOX_TURN_MODE"] = options.TurnMode.ToLowerInvariant();
            psi.EnvironmentVariables["KHARVOX_MOVEMENT_DIRECTION"] = options.MovementDirection;
            psi.EnvironmentVariables["KHARVOX_SMOOTH_TURN_SPEED"] = Inv(options.SmoothTurnSpeed);
            psi.EnvironmentVariables["KHARVOX_SNAP_TURN_ANGLE"] = Inv(options.SnapTurnAngle);
            psi.EnvironmentVariables["KHARVOX_TURN_DEADZONE"] = Inv(options.TurnDeadzone);
            psi.EnvironmentVariables["KHARVOX_MOUSE_COUNTS_PER_DEGREE"] = "8";
            psi.EnvironmentVariables["KHARVOX_WEAPON_6DOF"] = "1";
            psi.EnvironmentVariables["KHARVOX_TWO_HAND_MODE"] = options.TwoHandCalibration ? "calibration" : "gameplay-test";
            psi.EnvironmentVariables["KHARVOX_TWO_HAND_CALIBRATION_WEAPON"] = options.TwoHandCalibrationWeapon;
            psi.EnvironmentVariables["KHARVOX_TWO_HAND_ALIGNMENT"] = options.TwoHandAlignment;
            psi.EnvironmentVariables["KHARVOX_VIRTUAL_GUNSTOCK"] = options.VirtualGunstock ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_CAPTURE_EYES"] = options.CaptureEyes ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_DISABLE_AA"] = options.DisableAa ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_HANDS_JUMP"] = options.HandsJump ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_PHYSICAL_GLORYKILL"] = options.PhysicalGlorykill ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_PHYSICAL_GLORYKILL_SPEED"] = Inv(options.PhysicalGlorykillSpeed);
            psi.EnvironmentVariables["KHARVOX_PHYSICAL_GLORYKILL_HANDS"] = options.PhysicalGlorykillHands;
            psi.EnvironmentVariables["KHARVOX_LEFT_HANDED"] = options.LeftHanded ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_LEFT_HAND_SWAP"] = options.LeftHandSwapMode;
            psi.EnvironmentVariables["KHARVOX_BACK_WEAPON"] = options.BackWeapon;
            psi.EnvironmentVariables["KHARVOX_MOTION_WEAPON_WHEEL"] = "1";
            psi.EnvironmentVariables["KHARVOX_LASER_SIGHT"] = options.LaserSight ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_WEAPON_SCALE"] = "0.77";
            psi.EnvironmentVariables["KHARVOX_HANDS_PROJECTION_SCALE"] = "1";
            psi.EnvironmentVariables["KHARVOX_WORLD_SCALE"] = "39.3701";
            psi.EnvironmentVariables["KHARVOX_HUD_DEBUG"] = options.HudDebugging ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_SHOW_HANDS"] = options.ShowHands ? "1" : "0";
            psi.EnvironmentVariables["KHARVOX_HAND_CALIBRATION"] = options.HandCalibrationMode;
            psi.EnvironmentVariables["KHARVOX_RENDER_SCALE"] = Inv(effectiveRenderScale / 100m);
            // Direct SteamVR keeps the WSI carrier at 100%, but the FSR test
            // still needs the selected/native DOOM scale to decide whether the
            // optional EASU+RCAS pass is requested.
            psi.EnvironmentVariables["KHARVOX_FSR_SOURCE_SCALE"] =
                Inv(effectiveRenderScale / 100m);
            psi.EnvironmentVariables["KHARVOX_USE_FSR1"] = fsr1Enabled ? "1" : "0";
            BhapticsBridgeSession.ApplyToGame(psi, bhaptics);
            Psvr2BridgeSession.ApplyToGame(psi, psvr2);
            var logPath = Path.Combine(Path.GetTempPath(), "KHARVOX.log");
            // Remove a stale capture even when logging is disabled so a later
            // support request cannot accidentally attach an older run.
            try { File.Delete(loaderLogPath); } catch { }
            if (File.Exists(logPath))
            {
                try { File.Copy(logPath, Path.Combine(Path.GetTempPath(), "KHARVOX-previous-launch.log"), true); }
                catch { }
            }
            var nativeValidationLog = NativeValidation.Configure(psi, nativeStereoEnabled, runtimeDir);
            try { File.Delete(logPath); } catch { }
            File.AppendAllText(logPath,
                "[KHARVOX][LAUNCHER] build=" + BuildId + " runtimeDir=" + runtimeDir +
                " requestedRenderer=" + options.RendererMode +
                " nativeAttempt=" + nativeStereoEnabled +
                " previousNativeFailure=\"" + (previousNativeFailure ?? "none") + "\"" +
                " twoHandMode=" + (options.TwoHandCalibration ? "calibration" : "gameplay-test") +
                " calibrationWeapon=" + options.TwoHandCalibrationWeapon +
                " backWeapon=" + options.BackWeapon +
                " alignment=" + options.TwoHandAlignment +
                " virtualGunstock=" + (options.VirtualGunstock ? "enabled" : "disabled") +
                " physicalGlorykill=" + (options.PhysicalGlorykill ? "enabled" : "disabled") +
                " punchSpeed=" + Inv(options.PhysicalGlorykillSpeed) + "m/s" +
                " punchHands=" + options.PhysicalGlorykillHands +
                " handedness=" + (options.LeftHanded ? "left" : "right") +
                " leftHandSwap=" + (options.LeftHanded ? options.LeftHandSwapMode : "none") +
                " movementDirection=" + options.MovementDirection +
                " laserSight=" + (options.LaserSight ? "enabled" : "disabled") +
                " mouseInput=disabled" +
                " hudCalibration=repository-default" +
                " hudDebugging=" + (options.HudDebugging ? "enabled" : "disabled") +
                " extendedLogging=" + (options.ExtendedLogging ? "enabled" : "disabled") +
                " handsJump=" + (options.HandsJump ? "enabled" : "disabled") +
                " showHands=" + (options.ShowHands ? "enabled" : "disabled") +
                " handCalibration=" + options.HandCalibrationMode +
                " immersiveMode=" + (options.ImmersiveMode ? "enabled" : "disabled") +
                " cinematicFreelook=" + (options.CinematicFreelook ? "enabled" : "disabled") +
                " otherCinematicsInQuad=" + (options.OtherCinematicsInQuad ? "enabled" : "disabled") +
                " cinewindowFollowsHeadset=" + (options.CinewindowFollowsHeadset ? "enabled" : "disabled") +
                " bHaptics=" + (bhaptics is null ? "inactive" : "bridge-started") +
                " psvr2Toolkit=" + (psvr2 is null ? "inactive" : "bridge-started") +
                " steamVrLayerIsolation=" + (steamVrLayerIsolation ? "enabled" : "inactive") +
                " renderScaleSelected=" + Inv(options.RenderScale) + "%" +
                " renderScaleEffective=" + Inv(effectiveRenderScale) + "%" +
                " desktopPresentation=headset-source-independent-desktop" +
                " steamVrRenderScaleOverride=inactive" +
                " steamVrNativeSourceScale=" +
                    (steamVrLayerIsolation ? Inv(steamNativeSourceScale) + "%" : "inactive") +
                " steamVrAsyncReprojection=" + steamVrAsyncReprojection +
                " externalOpenXrApiLayer=" + (steamVrLayerIsolation ? "disabled" : "runtime-default") +
                " doomAsyncCompute=disabled renderer=" + (nativeStereoEnabled ? "NATIVE (experimental; headset unvalidated)" : "AER") +
                " fsr1=" + (fsr1Enabled ? "enabled" : "disabled") + Environment.NewLine +
                "[KHARVOX][LAUNCHER] gameArguments=" + psi.Arguments + Environment.NewLine);
            if (nativeValidationLog is not null)
                File.AppendAllText(logPath, "[KHARVOX][LAUNCHER] Native Vulkan validation: " + nativeValidationLog + Environment.NewLine);
            if (options.ExtendedLogging)
            {
                AppendDiagnostic(logPath, launchId,
                    "preflight " + preflight.StructuredLogEntry());
                AppendLaunchDiagnostics(logPath, launchId, 1, runtimeDir,
                    manifestPath, dllPath, gameExe, key);
                AppendSystemDiagnostics(logPath, launchId, preflight);
                AppendChildEnvironmentDiagnostics(logPath, launchId, psi);
                AppendDiagnostic(logPath, launchId,
                    "Vulkan loader stdout/stderr capture=" + loaderLogPath);
            }
            const long logOffset = 0;
            EnsureNoRunningDoom();
            if (options.ExtendedLogging)
                AppendRegistryDiagnostics(logPath, launchId, "before-process-start", key,
                    manifestPath);
            var startedProcess = Process.Start(psi);
            if (options.ExtendedLogging && startedProcess is not null)
            {
                AttachVulkanLoaderCapture(startedProcess, loaderLogPath, launchId);
                try
                {
                    startedProcess.BeginOutputReadLine();
                    startedProcess.BeginErrorReadLine();
                }
                catch (InvalidOperationException) { }
                AppendDiagnostic(logPath, launchId,
                    "Process.Start returned pid=" + startedProcess.Id.ToString(Invariant));
            }
            else if (options.ExtendedLogging)
                AppendDiagnostic(logPath, launchId, "Process.Start returned null");
            else startedProcess?.Dispose();

            var deadline = DateTime.UtcNow.AddSeconds(45);
            Process? game = null;
            try
            {
                if (options.ExtendedLogging && startedProcess is not null && !startedProcess.HasExited
                    && string.Equals(startedProcess.ProcessName, "DOOMx64vk",
                        StringComparison.OrdinalIgnoreCase))
                    game = startedProcess;
            }
            catch { }
            while (game is null && DateTime.UtcNow < deadline)
            {
                await Task.Delay(500);
                game = Process.GetProcessesByName("DOOMx64vk").FirstOrDefault();
            }
            if (game is null) throw new InvalidOperationException("DOOM did not start within 45 seconds.");
            if (options.ExtendedLogging)
                AppendDiagnostic(logPath, launchId,
                    "tracked DOOM pid=" + game.Id.ToString(Invariant)
                    + " returnedPid=" + (startedProcess?.Id.ToString(Invariant) ?? "none")
                    + " registryStillPresent=" + RegistryValueMatches(key, manifestPath));
            var gameProcessStart = DateTime.UtcNow;
            try
            {
                game.EnableRaisingEvents = true;
                game.Exited += (_, _) =>
                {
                    try
                    {
                        var exitCode = game.ExitCode;
                        var uptimeSeconds = Math.Max(0,
                            (DateTime.UtcNow - gameProcessStart).TotalSeconds);
                        File.AppendAllText(logPath,
                            "[KHARVOX][LAUNCHER] DOOM process exited code=0x" +
                            exitCode.ToString("X8", Invariant) +
                            " uptimeSeconds=" + uptimeSeconds.ToString("F1", Invariant) +
                            " runtime=" + new DirectoryInfo(runtimeDir).Name +
                            Environment.NewLine);
                    }
                    catch { /* Exit telemetry must never affect process cleanup. */ }
                };
            }
            catch { /* Process-exit telemetry is diagnostic only. */ }
            currentGame?.Dispose();
            currentGame = game;
            await FocusDoomOnceWhenWindowReadyAsync(game, logPath);
            statusUpdate?.Invoke("DOOM started. Verifying the VR image …");

            var startup = await Task.Run(() =>
                WaitForStartup(game, logPath, logOffset, TimeSpan.FromSeconds(35),
                    "runtimeDir=" + Path.Combine(runtimeDir, "."),
                    options.ExtendedLogging, launchId));
            if (startup == StartupState.Healthy)
            {
                // FSR1 writes its ACTIVE status only after the
                // renderer has actually initialized. Do not turn a successful
                // game start into a false-positive feature status here.
                if (!nativeStereoEnabled && !fsr1Enabled)
                    WriteRendererStatus(runtimeDir, "Renderer: AER 0.8 Beta");
                gameDetected?.Invoke();
                launchSucceeded = true;
                return;
            }

            // A SYNCHRONIZED OpenXR session with shouldRender=false is a
            // runtime visibility/focus state. Restarting DOOM cannot make
            // the runtime promote that session to VISIBLE or FOCUSED, and
            // killing a responsive process looks like a gameplay crash.
            // Keep the process alive so the runtime can recover naturally.
            if (startup == StartupState.RuntimeNotRendering)
            {
                if (options.ExtendedLogging)
                    AppendDiagnostic(logPath, launchId,
                        "OpenXR session synchronized with shouldRender=false; preserving responsive DOOM process and skipping automatic restart");
                WriteRendererStatus(runtimeDir, nativeStereoEnabled
                    ? "Renderer: Native Stereo EXP (OpenXR runtime not visible; waiting for frames)"
                    : "Renderer: OpenXR runtime not visible; waiting for frames");
                statusUpdate?.Invoke("OpenXR session is running but the runtime is not presenting frames. DOOM remains running; return focus to the headset/runtime.");
                gameDetected?.Invoke();
                launchSucceeded = true;
                return;
            }

            if (PreserveGameAfterUnconfirmedStartup(startup))
            {
                AppendDiagnostic(logPath, launchId,
                    "startup unconfirmed state=" + startup + "; preserving DOOM process");
                WriteRendererStatus(runtimeDir, "Renderer: startup not confirmed; DOOM remains running");
                statusUpdate?.Invoke("VR startup could not be confirmed. DOOM remains running; check the headset and runtime.");
                gameDetected?.Invoke();
                launchSucceeded = true;
                return;
            }

            var processExited = startup == StartupState.Exited;
            var headsetUnavailable = processExited && game.ExitCode == HeadsetUnavailableException.ExitCode;
            var memoryCapacityExceeded = processExited &&
                game.ExitCode == RenderMemoryCapacityException.ExitCode;
            if (!processExited)
            {
                if (options.ExtendedLogging)
                    AppendDiagnostic(logPath, launchId,
                        "launcher terminating DOOM after startup state=" + startup);
                await StopGameProcessAsync(game);
            }
            currentGame = null;
            game.Dispose();
            if (headsetUnavailable) throw new HeadsetUnavailableException();
            if (memoryCapacityExceeded)
                throw new RenderMemoryCapacityException();
            var failureMessage = processExited
                ? "DOOM exited before a stable VR frame stream was confirmed."
                : startup == StartupState.DeviceLost
                    ? "SteamVR lost the Vulkan device during startup. Restart SteamVR and try once more."
                : startup == StartupState.PresentStreamStalled
                    ? "DOOM's Vulkan present stream stalled during startup. " +
                      "Restart the OpenXR runtime and try once more."
                    : "DOOM did not establish a stable VR frame stream. " +
                      "Restart the OpenXR runtime and try once more.";
            if (options.ExtendedLogging)
            {
                try
                {
                    var loaderLog = File.Exists(loaderLogPath)
                        ? File.ReadAllText(loaderLogPath) : string.Empty;
                    var layerLoaded = VulkanLoaderStartupDiagnosis.ShowsKharvoxLayer(loaderLog);
                    AppendDiagnostic(logPath, launchId,
                        "startupTimeout kharvoxLayerLoaded=" +
                        (layerLoaded ? "true" : "false") +
                        " elevationProven=" +
                        (preflight.HasProvenElevationEvidence ? "true" : "false"));
                    failureMessage = VulkanLoaderStartupDiagnosis.AddToFailureMessage(
                        failureMessage, loaderLog,
                        preflight.HasProvenElevationEvidence);
                }
                catch { /* Loader-log diagnosis must not hide the startup failure. */ }
            }
            throw new InvalidOperationException(failureMessage);

        }
        finally
        {
            try
            {
                using var userHive = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry64);
                using var key = userHive.OpenSubKey(RegistryPath, true);
                if (options.ExtendedLogging && key is not null)
                    AppendRegistryDiagnostics(Path.Combine(Path.GetTempPath(), "KHARVOX.log"),
                        launchId, "before-final-cleanup", key, manifestPath);
                if (key is not null) DisableKharvoxLayerRegistrations(key, string.Empty);
                if (options.ExtendedLogging && key is not null)
                    AppendRegistryDiagnostics(Path.Combine(Path.GetTempPath(), "KHARVOX.log"),
                        launchId, "after-final-cleanup", key, manifestPath);
            }
            catch { }
            foreach (var path in temporaryFiles)
            {
                try { File.Delete(path); } catch { }
            }
            if (!launchSucceeded)
            {
                await StopBhapticsAsync().ConfigureAwait(false);
                await StopPsvr2Async().ConfigureAwait(false);
            }
        }
    }

    public static bool IsNvidiaGpu()
    {
        var forced = Environment.GetEnvironmentVariable("KHARVOX_GPU_VENDOR");
        if (!string.IsNullOrWhiteSpace(forced))
            return forced.Equals("NVIDIA", StringComparison.OrdinalIgnoreCase);
        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT AdapterCompatibility, Name FROM Win32_VideoController");
            foreach (ManagementObject adapter in searcher.Get())
            {
                using (adapter)
                {
                    var identity = $"{adapter["AdapterCompatibility"]} {adapter["Name"]}";
                    if (identity.IndexOf("NVIDIA", StringComparison.OrdinalIgnoreCase) >= 0) return true;
                }
            }
        }
        catch { /* Missing WMI data means the universal cached-eye fallback. */ }
        return false;
    }

    public static string DetectedRendererDescription() => "Renderer: AER (default)";

    private static void WriteRendererStatus(string runtimeDir, string value)
    {
        try { File.WriteAllText(Path.Combine(runtimeDir, "renderer_status.txt"), value); }
        catch { /* Renderer status is informational and must never block launch. */ }
    }

    private static void WriteTwoHandStatus(string runtimeDir, string value)
    {
        try { File.WriteAllText(Path.Combine(runtimeDir, "two_hand_status.txt"), value); }
        catch { /* Weapon status is informational and must never block launch. */ }
    }

    internal static void EnableLayerForGame(ProcessStartInfo startInfo)
    {
        startInfo.EnvironmentVariables["KHARVOX_ENABLE_LAYER"] = "1";
        startInfo.EnvironmentVariables.Remove("KHARVOX_DISABLE_LAYER");
    }

    internal static void DisableLayerRegistrationsOnExit()
    {
        // Do not interfere with a different launcher currently starting DOOM.
        FileStream? gate = null;
        try
        {
            if (Volatile.Read(ref launchInProgress) == 0) gate = AcquireLaunchGate();
            using var userHive = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, RegistryView.Registry64);
            using var key = userHive.OpenSubKey(RegistryPath, true);
            if (key is not null) DisableKharvoxLayerRegistrations(key, string.Empty);
        }
        catch { /* Exit cleanup is best effort; the manifest also requires process-local opt-in. */ }
        finally { gate?.Dispose(); }
    }

    internal static void DisableKharvoxLayerRegistrations(RegistryKey key, string currentManifestPath)
    {
        foreach (var valueName in key.GetValueNames())
        {
            if (string.Equals(valueName, currentManifestPath, StringComparison.OrdinalIgnoreCase)) continue;
            if (!string.Equals(Path.GetFileName(valueName), "KharvoxLayer.json", StringComparison.OrdinalIgnoreCase)) continue;

            var isKharvoxManifest = valueName.IndexOf("Kharvox", StringComparison.OrdinalIgnoreCase) >= 0;
            try
            {
                if (File.Exists(valueName))
                    isKharvoxManifest = File.ReadAllText(valueName).IndexOf(LayerName, StringComparison.Ordinal) >= 0;
            }
            catch { }

            if (isKharvoxManifest) key.SetValue(valueName, 1, RegistryValueKind.DWord);
        }
        key.Flush();
    }

    private static void AppendLaunchDiagnostics(string logPath, string launchId, int attempt,
        string runtimeDir, string manifestPath, string dllPath, string gameExe, RegistryKey key)
    {
        AppendDiagnostic(logPath, launchId,
            "attempt=" + attempt.ToString(Invariant)
            + " launcherPid=" + Process.GetCurrentProcess().Id.ToString(Invariant)
            + " process64=" + Environment.Is64BitProcess
            + " os64=" + Environment.Is64BitOperatingSystem
            + " runtimeExists=" + Directory.Exists(runtimeDir)
            + " gameExists=" + File.Exists(gameExe));
        AppendDiagnostic(logPath, launchId,
            "layer dll exists=" + File.Exists(dllPath)
            + " bytes=" + FileLength(dllPath)
            + " sha256=" + FileSha256(dllPath)
            + " zoneIdentifier=" + HasZoneIdentifier(dllPath));
        AppendDiagnostic(logPath, launchId,
            "manifest exists=" + File.Exists(manifestPath)
            + " bytes=" + FileLength(manifestPath)
            + " sha256=" + FileSha256(manifestPath)
            + " zoneIdentifier=" + HasZoneIdentifier(manifestPath));
        foreach (var name in new[] {
            "VK_INSTANCE_LAYERS", "VK_LAYER_PATH", "VK_IMPLICIT_LAYER_PATH",
            "VK_LOADER_LAYERS_DISABLE", "VK_LOADER_LAYERS_ALLOW",
            "KHARVOX_DISABLE_LAYER"
        })
        {
            var value = Environment.GetEnvironmentVariable(name);
            AppendDiagnostic(logPath, launchId,
                "parent env " + name + "=" + (string.IsNullOrEmpty(value) ? "<unset>" : value));
        }
        AppendRegistryDiagnostics(logPath, launchId, "attempt-start", key, manifestPath);
    }

    private static void AppendSystemDiagnostics(string logPath, string launchId,
        LaunchPreflightResult preflight)
    {
        AppendDiagnostic(logPath, launchId,
            "system os=" + Environment.OSVersion.VersionString
            + " framework=" + Environment.Version
            + " elevated=" + LaunchPreflight.LogValue(preflight.LauncherElevation)
            + " processors=" + Environment.ProcessorCount.ToString(Invariant)
            + " systemPageSize=" + Environment.SystemPageSize.ToString(Invariant));

        try
        {
            using var machine = RegistryKey.OpenBaseKey(
                RegistryHive.LocalMachine, RegistryView.Registry64);
            using var graphics = machine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Control\GraphicsDrivers", false);
            AppendDiagnostic(logPath, launchId,
                "graphicsDrivers HwSchMode=" +
                (Convert.ToString(graphics?.GetValue("HwSchMode"), Invariant) ?? "default"));
        }
        catch (Exception exception)
        {
            AppendDiagnostic(logPath, launchId,
                "graphicsDrivers readError=" + exception.GetType().Name);
        }

        try
        {
            using var machine = RegistryKey.OpenBaseKey(
                RegistryHive.LocalMachine, RegistryView.Registry64);
            using var openXr = machine.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1", false);
            var activeRuntime = Convert.ToString(openXr?.GetValue("ActiveRuntime"), Invariant);
            AppendDiagnostic(logPath, launchId,
                "OpenXR ActiveRuntime=" +
                (string.IsNullOrWhiteSpace(activeRuntime) ? "<missing>" : activeRuntime));
        }
        catch (Exception exception)
        {
            AppendDiagnostic(logPath, launchId,
                "OpenXR runtime registry readError=" + exception.GetType().Name);
        }

        try
        {
            var screens = System.Windows.Forms.Screen.AllScreens.Select(screen =>
                screen.DeviceName
                + ":bounds=" + screen.Bounds.X.ToString(Invariant) + ","
                + screen.Bounds.Y.ToString(Invariant) + ","
                + screen.Bounds.Width.ToString(Invariant) + "x"
                + screen.Bounds.Height.ToString(Invariant)
                + ":work=" + screen.WorkingArea.X.ToString(Invariant) + ","
                + screen.WorkingArea.Y.ToString(Invariant) + ","
                + screen.WorkingArea.Width.ToString(Invariant) + "x"
                + screen.WorkingArea.Height.ToString(Invariant)
                + ":primary=" + screen.Primary);
            AppendDiagnostic(logPath, launchId, "displays " + string.Join(";", screens));
        }
        catch (Exception exception)
        {
            AppendDiagnostic(logPath, launchId,
                "display enumeration error=" + exception.GetType().Name);
        }

        try
        {
            using var searcher = new ManagementObjectSearcher(
                "SELECT Name, DriverVersion, CurrentHorizontalResolution, CurrentVerticalResolution, CurrentRefreshRate FROM Win32_VideoController");
            foreach (ManagementObject adapter in searcher.Get())
            {
                using (adapter)
                    AppendDiagnostic(logPath, launchId,
                        "gpu name=" + Convert.ToString(adapter["Name"], Invariant)
                        + " driver=" + Convert.ToString(adapter["DriverVersion"], Invariant)
                        + " desktop=" + Convert.ToString(adapter["CurrentHorizontalResolution"], Invariant)
                        + "x" + Convert.ToString(adapter["CurrentVerticalResolution"], Invariant)
                        + " hz=" + Convert.ToString(adapter["CurrentRefreshRate"], Invariant));
            }
        }
        catch (Exception exception)
        {
            AppendDiagnostic(logPath, launchId,
                "gpu enumeration error=" + exception.GetType().Name);
        }

        foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
        {
            try
            {
                using var baseKey = RegistryKey.OpenBaseKey(hive, RegistryView.Registry64);
                using var layers = baseKey.OpenSubKey(RegistryPath, false);
                if (layers is null)
                {
                    AppendDiagnostic(logPath, launchId,
                        "Vulkan implicit layers hive=" + hive + " state=<missing>");
                    continue;
                }
                foreach (var valueName in layers.GetValueNames())
                    AppendDiagnostic(logPath, launchId,
                        "Vulkan implicit layer hive=" + hive
                        + " enabledValue=" + Convert.ToString(layers.GetValue(valueName), Invariant)
                        + " manifest=" + valueName);
            }
            catch (Exception exception)
            {
                AppendDiagnostic(logPath, launchId,
                    "Vulkan implicit layers hive=" + hive
                    + " readError=" + exception.GetType().Name);
            }
        }
    }

    private static void AppendChildEnvironmentDiagnostics(string logPath, string launchId,
        ProcessStartInfo processStart)
    {
        foreach (var name in new[] {
            "KHARVOX_EXTENDED_LOGGING", "KHARVOX_SHOW_HANDS",
            "KHARVOX_HAND_CALIBRATION", "VK_LOADER_DEBUG",
            "DISABLE_VK_LAYER_VALVE_steam_overlay_1",
            "DISABLE_VK_LAYER_VALVE_steam_fossilize_1",
            "DISABLE_VULKAN_OBS_CAPTURE", "EOS_OVERLAY_DISABLE_VULKAN_WIN64",
            "DISABLE_XR_APILAYER_VIRTUALDESKTOP_OCULUS_COMPATIBILITY"
        })
        {
            var value = processStart.EnvironmentVariables[name];
            AppendDiagnostic(logPath, launchId,
                "child env " + name + "=" +
                (string.IsNullOrEmpty(value) ? "<unset>" : value));
        }
    }

    private static void AppendRegistryDiagnostics(string logPath, string launchId,
        string stage, RegistryKey key, string manifestPath)
    {
        try
        {
            var value = key.GetValue(manifestPath, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            var kind = value is null ? "missing" : key.GetValueKind(manifestPath).ToString();
            var matches = key.GetValueNames().Count(name =>
                string.Equals(Path.GetFileName(name), "KharvoxLayer.json",
                    StringComparison.OrdinalIgnoreCase));
            AppendDiagnostic(logPath, launchId,
                "registry stage=" + stage
                + " hive=HKCU view=Registry64 key=" + RegistryPath
                + " current=" + (value is null ? "missing" : Convert.ToString(value, Invariant))
                + " kind=" + kind
                + " kharvoxManifestCount=" + matches.ToString(Invariant));
        }
        catch (Exception exception)
        {
            AppendDiagnostic(logPath, launchId,
                "registry stage=" + stage + " readError=" + exception.GetType().Name);
        }
    }

    private static string RegistryValueMatches(RegistryKey key, string manifestPath)
    {
        try
        {
            var value = key.GetValue(manifestPath, null, RegistryValueOptions.DoNotExpandEnvironmentNames);
            return value is int integer && integer == 0 ? "yes" : "no";
        }
        catch (Exception exception) { return "error:" + exception.GetType().Name; }
    }

    private static void AttachVulkanLoaderCapture(Process process, string path, string launchId)
    {
        var lines = 0;
        void Capture(string stream, DataReceivedEventArgs args)
        {
            if (args.Data is null) return;
            var line = Interlocked.Increment(ref lines);
            if (line > 25000) return;
            lock (diagnosticLogLock)
            {
                try
                {
                    if (line == 1)
                        File.AppendAllText(path,
                            "[KHARVOX][EXTENDED][" + launchId + "] pid="
                            + process.Id.ToString(Invariant) + Environment.NewLine);
                    File.AppendAllText(path, "[" + stream + "] " + args.Data + Environment.NewLine);
                    if (line == 25000)
                        File.AppendAllText(path, "[CAPTURE] output truncated at 25000 lines" + Environment.NewLine);
                }
                catch { }
            }
        }
        process.OutputDataReceived += (_, args) => Capture("stdout", args);
        process.ErrorDataReceived += (_, args) => Capture("stderr", args);
    }

    private static void AppendDiagnostic(string logPath, string launchId, string message)
    {
        lock (diagnosticLogLock)
        {
            try
            {
                File.AppendAllText(logPath,
                    "[KHARVOX][EXTENDED][" + launchId + "] " + message + Environment.NewLine);
            }
            catch { }
        }
    }

    private static long FileLength(string path)
    {
        try { return new FileInfo(path).Length; }
        catch { return -1; }
    }

    private static string FileSha256(string path)
    {
        try
        {
            using var stream = File.OpenRead(path);
            using var sha = SHA256.Create();
            return string.Concat(sha.ComputeHash(stream).Select(value => value.ToString("x2", Invariant)));
        }
        catch (Exception exception) { return "error:" + exception.GetType().Name; }
    }

    private static string HasZoneIdentifier(string path)
    {
        try { return File.Exists(path + ":Zone.Identifier") ? "present" : "absent"; }
        catch (Exception exception) { return "error:" + exception.GetType().Name; }
    }

    public static async Task EndDoomAsync()
    {
        var game = currentGame;
        if (game is null)
        {
            await StopBhapticsAsync().ConfigureAwait(false);
            await StopPsvr2Async().ConfigureAwait(false);
            return;
        }
        try
        {
            await StopGameProcessAsync(game);
        }
        finally
        {
            currentGame = null;
            game.Dispose();
            await StopBhapticsAsync().ConfigureAwait(false);
            await StopPsvr2Async().ConfigureAwait(false);
        }
    }

    internal static async Task StopBhapticsAsync()
    {
        BhapticsBridgeSession? session;
        lock (bhapticsLock)
        {
            session = currentBhaptics;
            currentBhaptics = null;
        }
        if (session is not null)
            await session.StopAsync().ConfigureAwait(false);
    }

    private static void StopBhapticsInBackground()
    {
        BhapticsBridgeSession? session;
        lock (bhapticsLock)
        {
            session = currentBhaptics;
            currentBhaptics = null;
        }
        if (session is not null)
            _ = Task.Run(session.StopAsync);
    }

    internal static async Task StopPsvr2Async()
    {
        Psvr2BridgeSession? session;
        lock (psvr2Lock)
        {
            session = currentPsvr2;
            currentPsvr2 = null;
        }
        if (session is not null)
            await session.StopAsync().ConfigureAwait(false);
    }

    private static void StopPsvr2InBackground()
    {
        Psvr2BridgeSession? session;
        lock (psvr2Lock)
        {
            session = currentPsvr2;
            currentPsvr2 = null;
        }
        if (session is not null)
            _ = Task.Run(session.StopAsync);
    }

    private static StartupState WaitForStartup(Process game, string logPath, long logOffset,
        TimeSpan timeout, string expectedLayerMarker, bool extendedLogging, string launchId)
    {
        var deadline = DateTime.UtcNow + timeout;
        var lastLogLength = logOffset;
        var sawPresent = false;
        var runtimeNotRendering = false;
        var lastPipelineMarker = string.Empty;
        var lastPipelineProgress = DateTime.UtcNow;
        var nextProcessSnapshot = DateTime.MinValue;
        var processSample = 0;

        while (DateTime.UtcNow < deadline)
        {
            try { if (game.HasExited) return StartupState.Exited; }
            catch { return StartupState.Exited; }

            if (extendedLogging && DateTime.UtcNow >= nextProcessSnapshot)
            {
                AppendProcessSnapshot(logPath, launchId, game, ++processSample, "startup-watchdog");
                nextProcessSnapshot = DateTime.UtcNow.AddSeconds(2);
            }

            Thread.Sleep(250);
            try
            {
                var currentLength = GetFileLength(logPath);
                if (currentLength < logOffset)
                {
                    logOffset = 0;
                    lastLogLength = 0;
                    sawPresent = false;
                    lastPipelineMarker = string.Empty;
                    lastPipelineProgress = DateTime.UtcNow;
                }
                if (currentLength > lastLogLength)
                {
                    lastLogLength = currentLength;
                }
                var logState = ReadStartupLogState(logPath, logOffset, expectedLayerMarker);
                if (logState == StartupState.Healthy) return StartupState.Healthy;
                if (logState == StartupState.DeviceLost) return StartupState.DeviceLost;
                runtimeNotRendering = logState == StartupState.RuntimeNotRendering;
                sawPresent |= logState == StartupState.PresentObserved;
                var pipelineMarker = ReadLastPipelineMarker(logPath, logOffset);
                if (!string.Equals(pipelineMarker, lastPipelineMarker, StringComparison.Ordinal))
                {
                    lastPipelineMarker = pipelineMarker;
                    lastPipelineProgress = DateTime.UtcNow;
                }
            }
            catch (FileNotFoundException) { }
            catch (DirectoryNotFoundException) { }
            catch (IOException) { }

            if (sawPresent && !runtimeNotRendering
                && DateTime.UtcNow - lastPipelineProgress >= TimeSpan.FromSeconds(10))
            {
                if (extendedLogging)
                {
                    AppendDiagnostic(logPath, launchId,
                        "startup present stream stalled lastPipeline=" +
                        (string.IsNullOrEmpty(lastPipelineMarker) ? "<none>" : lastPipelineMarker));
                    AppendProcessSnapshot(logPath, launchId, game, ++processSample,
                        "present-stream-stalled");
                }
                return StartupState.PresentStreamStalled;
            }
        }
        if (extendedLogging)
            AppendProcessSnapshot(logPath, launchId, game, ++processSample, "startup-timeout");
        return runtimeNotRendering ? StartupState.RuntimeNotRendering : StartupState.TimedOut;
    }

    private static void AppendProcessSnapshot(string logPath, string launchId,
        Process game, int sample, string reason)
    {
        try
        {
            game.Refresh();
            var window = FindDoomWindow(game);
            var foreground = GetForegroundWindow();
            GetWindowThreadProcessId(foreground, out var foregroundPid);
            var windowPid = 0u;
            var windowThread = window == IntPtr.Zero
                ? 0u : GetWindowThreadProcessId(window, out windowPid);
            var rect = new NativeRect();
            var client = new NativeRect();
            var rectOk = window != IntPtr.Zero && GetWindowRect(window, out rect);
            var clientOk = window != IntPtr.Zero && GetClientRect(window, out client);
            AppendDiagnostic(logPath, launchId,
                "process sample=" + sample.ToString(Invariant)
                + " reason=" + reason
                + " pid=" + game.Id.ToString(Invariant)
                + " exited=" + game.HasExited
                + " responding=" + game.Responding
                + " threads=" + game.Threads.Count.ToString(Invariant)
                + " cpuMs=" + game.TotalProcessorTime.TotalMilliseconds.ToString("F0", Invariant)
                + " workingSet=" + game.WorkingSet64.ToString(Invariant)
                + " privateBytes=" + game.PrivateMemorySize64.ToString(Invariant)
                + " hwnd=0x" + window.ToInt64().ToString("X", Invariant)
                + " windowPid=" + windowPid.ToString(Invariant)
                + " windowThread=" + windowThread.ToString(Invariant)
                + " visible=" + (window != IntPtr.Zero && IsWindowVisible(window))
                + " iconic=" + (window != IntPtr.Zero && IsIconic(window))
                + " zoomed=" + (window != IntPtr.Zero && IsZoomed(window))
                + " hung=" + (window != IntPtr.Zero && IsHungAppWindow(window))
                + " foreground=" + (window != IntPtr.Zero && foreground == window)
                + " foregroundPid=" + foregroundPid.ToString(Invariant)
                + " rect=" + (rectOk
                    ? rect.Left.ToString(Invariant) + "," + rect.Top.ToString(Invariant) + ","
                      + rect.Right.ToString(Invariant) + "," + rect.Bottom.ToString(Invariant)
                    : "unavailable")
                + " client=" + (clientOk
                    ? (client.Right - client.Left).ToString(Invariant) + "x"
                      + (client.Bottom - client.Top).ToString(Invariant)
                    : "unavailable"));
        }
        catch (Exception exception)
        {
            AppendDiagnostic(logPath, launchId,
                "process sample=" + sample.ToString(Invariant)
                + " reason=" + reason
                + " error=" + exception.GetType().Name);
        }
    }

    private static StartupState ReadStartupLogState(string logPath, long logOffset, string expectedLayerMarker)
    {
        using var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        if (stream.Length <= logOffset) return StartupState.TimedOut;
        stream.Position = logOffset;
        var bytesToRead = checked((int)Math.Min(stream.Length - logOffset, 1024 * 1024));
        var buffer = new byte[bytesToRead];
        var totalRead = 0;
        while (totalRead < buffer.Length)
        {
            var read = stream.Read(buffer, totalRead, buffer.Length - totalRead);
            if (read == 0) break;
            totalRead += read;
        }
        return AnalyzeStartupLog(Encoding.UTF8.GetString(buffer, 0, totalRead), expectedLayerMarker);
    }

    // Missing/stalled diagnostics are not proof that a live game has failed.
    // Preserve it even if the log is unavailable or startup is unusually slow.
    internal static bool PreserveGameAfterUnconfirmedStartup(StartupState state) =>
        state == StartupState.TimedOut || state == StartupState.PresentStreamStalled;

    internal static StartupState AnalyzeStartupLog(string text, string expectedLayerMarker)
    {
        if ((text.Contains("copy submit failed -4") || text.Contains("copy fence wait failed -4")
                || text.Contains("copy completion failed -4")
                || text.Contains("VK_ERROR_DEVICE_LOST")
                || text.Contains("[SUBMIT] return") && text.Contains("result=-4")
                || text.Contains("[SUBMIT2] return") && text.Contains("result=-4"))
            && text.IndexOf(expectedLayerMarker, StringComparison.OrdinalIgnoreCase) >= 0)
            return StartupState.DeviceLost;
        if (text.Contains("Frame 120 stable mode=")
            && text.IndexOf(expectedLayerMarker, StringComparison.OrdinalIgnoreCase) >= 0)
            return StartupState.Healthy;
        var lastShouldNotRender = text.LastIndexOf("shouldRender=0", StringComparison.Ordinal);
        var lastShouldRender = text.LastIndexOf("shouldRender=1", StringComparison.Ordinal);
        if (text.Contains("[KHARVOX][XR] State -> 3")
            && lastShouldNotRender >= 0
            && lastShouldNotRender > lastShouldRender
            && text.IndexOf(expectedLayerMarker, StringComparison.OrdinalIgnoreCase) >= 0)
            return StartupState.RuntimeNotRendering;
        return text.Contains("[Present ")
            ? StartupState.PresentObserved
            : StartupState.TimedOut;
    }

    private static string ReadLastPipelineMarker(string logPath, long logOffset)
    {
        using var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        if (stream.Length <= logOffset) return string.Empty;
        stream.Position = Math.Max(logOffset, stream.Length - 256 * 1024);
        using var reader = new StreamReader(stream, Encoding.UTF8, true, 4096, true);
        string last = string.Empty;
        while (reader.ReadLine() is { } line)
        {
            if (line.Contains("[ACQUIRE]") || line.Contains("[ACQUIRE2]")
                || line.Contains("[PRESENT]") || line.Contains("[Frame ")
                || line.Contains("[Present ") || line.Contains("Swapchain created")
                || line.Contains("Swapchain destroyed") || line.Contains("xrBeginSession"))
                last = line;
        }
        return last;
    }

    private static long GetFileLength(string path)
    {
        try { return new FileInfo(path).Length; }
        catch { return 0; }
    }

    private static bool LogContains(string path, string value)
    {
        try { return File.ReadAllText(path).IndexOf(value, StringComparison.Ordinal) >= 0; }
        catch { return false; }
    }

    private static async Task StopGameProcessAsync(Process game)
    {
        try
        {
            if (game.HasExited) return;
            if (game.CloseMainWindow())
            {
                for (var i = 0; i < 20 && !game.HasExited; i++) await Task.Delay(100);
            }
            if (!game.HasExited) game.Kill();
            await Task.Run(() => game.WaitForExit());
        }
        catch (InvalidOperationException) { }
    }

    public static bool FocusDoom()
    {
        var game = currentGame;
        if (game is null) return false;
        try
        {
            if (game.HasExited) return false;
            var target = FindDoomWindow(game);
            return target != IntPtr.Zero && FocusWindow(target, game.Id);
        }
        catch (InvalidOperationException) { return false; }
    }

    private static async Task<bool> FocusDoomOnceWhenWindowReadyAsync(
        Process game, string logPath)
    {
        // Wait only for window creation; never retry focus or reclaim it later.
        var deadline = DateTime.UtcNow.AddSeconds(10);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                if (game.HasExited) return false;
                var target = FindDoomWindow(game);
                if (target != IntPtr.Zero)
                {
                    var foregroundBefore = ForegroundProcessId();
                    var focused = FocusWindow(target, game.Id);
                    AppendFocusLog(logPath, "startup-once", focused, foregroundBefore, 1);
                    return focused;
                }
            }
            catch (InvalidOperationException) { return false; }
            await Task.Delay(100);
        }
        AppendFocusLog(logPath, "startup-window-unavailable", false, ForegroundProcessId(), 0);
        return false;
    }

    private static IntPtr FindDoomWindow(Process game)
    {
        game.Refresh();
        IntPtr target = game.MainWindowHandle;
        if (target == IntPtr.Zero)
        {
            EnumWindows((window, _) => {
                GetWindowThreadProcessId(window, out var pid);
                if (pid == (uint)game.Id && IsWindowVisible(window))
                {
                    target = window;
                    return false;
                }
                return true;
            }, IntPtr.Zero);
        }
        return target;
    }

    private static bool FocusWindow(IntPtr target, int gameProcessId)
    {
        lock (focusOperationLock)
        {
            if (target == IntPtr.Zero) return false;
            GetWindowThreadProcessId(GetForegroundWindow(), out var foregroundPid);
            if (foregroundPid == (uint)gameProcessId) return true;

            AllowSetForegroundWindow((uint)gameProcessId);
            ShowWindowAsync(target, 9);
            var foreground = GetForegroundWindow();
            var foregroundThread = foreground == IntPtr.Zero
                ? 0u : GetWindowThreadProcessId(foreground, out _);
            var targetThread = GetWindowThreadProcessId(target, out _);
            var attached = false;
            try
            {
                if (foregroundThread != 0 && targetThread != 0
                    && foregroundThread != targetThread)
                    attached = AttachThreadInput(foregroundThread, targetThread, true);
                BringWindowToTop(target);
                SetForegroundWindow(target);
                SetFocus(target);
            }
            finally
            {
                if (attached) AttachThreadInput(foregroundThread, targetThread, false);
            }
            return ForegroundProcessId() == (uint)gameProcessId;
        }
    }

    private static uint ForegroundProcessId()
    {
        var foreground = GetForegroundWindow();
        if (foreground == IntPtr.Zero) return 0;
        GetWindowThreadProcessId(foreground, out var processId);
        return processId;
    }

    private static void AppendFocusLog(string logPath, string reason,
        bool focused, uint previousProcessId, int attempts)
    {
        try
        {
            File.AppendAllText(logPath,
                "[KHARVOX][LAUNCHER] DOOM foreground reason=" + reason
                + " result=" + (focused ? "focused" : "failed")
                + " previousPid=" + previousProcessId.ToString(Invariant)
                + " attempts=" + attempts.ToString(Invariant)
                + Environment.NewLine);
        }
        catch { }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsZoomed(IntPtr window);
    [DllImport("user32.dll")] private static extern bool IsHungAppWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr window, out NativeRect rect);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window, out NativeRect rect);
    [DllImport("user32.dll")] private static extern bool AllowSetForegroundWindow(uint processId);
    [DllImport("user32.dll")] private static extern bool AttachThreadInput(uint attach, uint attachTo, bool attachInput);
    [DllImport("user32.dll")] private static extern bool BringWindowToTop(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr SetFocus(IntPtr window);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] private static extern bool ShowWindowAsync(IntPtr window, int command);

    internal static decimal EffectiveRenderScale(decimal selectedRenderScale,
        bool steamVrRuntime) => Math.Max(50m, selectedRenderScale);

    internal static decimal SteamNativeResolutionScale(decimal selectedRenderScale,
        bool steamVrRuntime) => 100m; // Carrier already scales on every runtime.

    internal static int RenderDimension(int baseDimension, decimal percent)
    {
        // Divide before multiplying so even decimal.MaxValue fails cleanly.
        if (percent <= 0 || percent / 100m > int.MaxValue / (decimal)baseDimension)
            throw new InvalidOperationException("Requested render resolution exceeds the integer dimensions supported by DOOM/Windows. Reduce RenderScale.");
        return checked((int)Math.Round(baseDimension * (percent / 100m), MidpointRounding.AwayFromZero));
    }

    internal static IEnumerable<string> NativeShadowDiagnosticArguments(bool nativeStereoEnabled,
        bool uncachedShadowsRequested, bool cachedShadowsRequested = false)
    {
        // Private causal comparison only. The executable's CVar help defines
        // -1 as disabling static image caching for that mip. Keep preprocessed
        // caster geometry, atlas dimensions and the user's quality preset.
        if (!nativeStereoEnabled) return Array.Empty<string>();
        if (uncachedShadowsRequested && cachedShadowsRequested)
            throw new InvalidOperationException("Conflicting Native shadow-cache comparisons. Keep only one cache marker.");
        // Captured defaults from this supported executable, not stale-frame
        // limits: restoring the image cache must retain the freshness fix.
        if (cachedShadowsRequested)
            return ["+r_shadowStaticMode", "1", "+r_shadowStaticMipTriThresholds", "-1,16,8,4,2"];
        if (!uncachedShadowsRequested) return Array.Empty<string>();
        return ["+r_shadowStaticMode", "1", "+r_shadowStaticMipTriThresholds", "-1,-1,-1,-1,-1"];
    }

    internal static IEnumerable<string> NativeShadowFreshnessDiagnosticArguments(bool nativeStereoEnabled,
        bool freshShadowsRequested)
    {
        // Independent comparison: prevent lower-resolution shadow maps from
        // surviving skipped update frames. This may increase rendering work;
        // it is not a quality setting or an accepted performance fix.
        if (!nativeStereoEnabled || !freshShadowsRequested) return Array.Empty<string>();
        return ["+r_shadowMaxStaleFrames", "0,0,0,0,0",
            "+r_shadowParallelMaxStaleFrames", "0,0,0,0"];
    }

    internal static (int Width, int Height) DesktopMirrorExtent(int workWidth, int workHeight)
    {
        // Reserve space for the frame/taskbar and preserve the 16:9 mirror.
        var scale = Math.Min(1d, Math.Min(Math.Max(1L, (long)workWidth - 64) / 1280d,
            Math.Max(1L, (long)workHeight - 64) / 720d));
        return (Math.Max(1, (int)(1280 * scale)), Math.Max(1, (int)(720 * scale)));
    }

    private static IEnumerable<string> BuildGameArguments(KharvoxLaunchOptions options,
        bool useSteam, decimal effectiveRenderScale, bool nativeStereoEnabled = false)
    {
        var args = new List<string>();
        // AER and Native share a small desktop mirror on every runtime. The
        // paired layer obtains source resolution from OpenXR and RenderScale.
        var desktop = DesktopMirrorExtent(SystemInformation.WorkingArea.Width,
            SystemInformation.WorkingArea.Height);
        var renderWidth = desktop.Width;
        var renderHeight = desktop.Height;
        if (useSteam) args.AddRange(["-applaunch", "379720"]);
        // The simulator's Vulkan preview stalls when DOOM hands Present from
        // its startup thread to the SMP render thread. Keep its preview on the
        // inline path for every backend, including an AER recovery launch.
        if(nativeStereoEnabled || UsesSimulatorRuntime())
            args.AddRange(["+r_useSMP","1"]);
        args.AddRange(NativeShadowDiagnosticArguments(nativeStereoEnabled,
            true,
            File.Exists(Path.Combine(AppContext.BaseDirectory,"native_test_cached_shadows"))));
        args.AddRange(NativeShadowFreshnessDiagnosticArguments(nativeStereoEnabled,
            true));
        args.AddRange([
            "+r_renderAPI", "1", "+r_swapInterval", "0",
            // DOOM 2016 exposes this Vulkan CVar as its Compute Shaders
            // option. Keep its asynchronous compute/present path out of this
            // SteamVR cadence isolation while preserving every XR lifecycle
            // change from test 3b.
            "+r_enableAsyncCompute", "0",
            // DOOM 2016's native DirectInput mouse switch. Disable the mouse
            // before input-device initialization so neither gameplay view nor
            // Dossier/map UI can consume movement, buttons, or wheel events.
            // Keyboard, XInput and the OpenXR action path remain independent.
            "+in_mouse", "0",
            "+com_skipIntroVideo", "1", "+r_fullscreen", "0", "+r_mode", "19",
            "+g_showCrosshair", "0", "+r_motionblur", "0", "+r_motionBlurQuality", "0", "+g_autoMotionBlurOnGK", "0",
            // Flat-screen damage kicks and screen-view shakes rotate the
            // native body-camera basis underneath OpenXR's gravity-level HMD
            // pose. In a headset that appears as a displaced horizon. Use the
            // engine's dedicated skip CVars instead of altering camera, stereo
            // or turn transforms; damage, physics knockback and haptics remain.
            "+view_skipKicks", "1", "+view_skipShakes", "1",
            // Disable native view bob and weapon sway at their engine source.
            "+pm_noBob", "1",
            "+r_skipShadows", "0", // Restore shadows after the r285 comparison.
            // Make the selected RenderScale the actual render resolution;
            // DOOM's dynamic scaler must not silently lower it again.
            "+rs_enable", "0",
            "+rs_forceFractionX", "1", "+rs_forceFractionY", "1",
            "+rs_minimumResolutionScale", "1",
            "+r_chromaticAberration", "0", "+hands_show", "2", "+hands_show_mp", "2",
            // DOOM normally renders its first-person weapon at 0.1x depth so
            // walls cannot clip it. Static KHARVOX hands share the native
            // scene depth, so use one physical depth scale while they are
            // enabled. The disabled path preserves DOOM's original value.
            "+hands_depthHack", options.ShowHands ? "1" : "0.1",
            "+g_showPlayerShadow", "0", "+pmec_ForceShowThirdPersonBody", "0",
            "+pmec_lg_RequireLookAtLedge", "0", "+pmec_lg_AssumeAlwaysForwardPress", "1",
            // Use a linear, unsmoothed native carrier. The OpenXR layer maps
            // the requested smooth rate into XInput magnitude and measures
            // snap angles from DOOM's resulting player/body yaw.
            "+joy_yawSpeed", "600", "+joy_deadZone", "0.15",
            "+joy_dampenLook", "0", "+joy_gammaLook", "0",
            "+joy_smoothingEnabled", "0", "+joy_edgeAccelerationScalar", "0",
            // DOOM mode 2 is spatial SMAA; neither eye consumes temporal AA history.
            "+r_antialiasing", options.DisableAa ? "0" : "2", "+r_sharpening", "2",
            "+r_filmGrainRatio", "0",
            "+r_SSR", "0", "+r_SSRQuality", "0", // Lowest SSR quality; not a verified off switch.
            "+r_windowWidth", renderWidth.ToString(Invariant),
            "+r_windowHeight", renderHeight.ToString(Invariant)
        ]);
        return args;
    }

    private static string? ActiveRuntimeManifest()
    {
        var manifest = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
        if (string.IsNullOrWhiteSpace(manifest))
        {
            try
            {
                using var baseKey = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
                using var key = baseKey.OpenSubKey(@"SOFTWARE\Khronos\OpenXR\1");
                manifest = key?.GetValue("ActiveRuntime") as string;
            }
            catch { }
        }
        return string.IsNullOrWhiteSpace(manifest)
            ? null
            : Environment.ExpandEnvironmentVariables(manifest);
    }

    private static bool UsesPimaxRuntime() => IsPimaxRuntimeManifest(ActiveRuntimeManifest());

    internal static bool IsPimaxRuntimeManifest(string? manifest)
    {
        return manifest is not null
            && (manifest.IndexOf("pimax", StringComparison.OrdinalIgnoreCase) >= 0
                || manifest.IndexOf("piopenxr", StringComparison.OrdinalIgnoreCase) >= 0);
    }

    private static bool UsesVirtualDesktopRuntime()
    {
        var manifest = ActiveRuntimeManifest();
        if (manifest is null) return false;
        return manifest.IndexOf("virtualdesktop", StringComparison.OrdinalIgnoreCase) >= 0
            || manifest.IndexOf("virtual desktop", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    internal static bool IsSimulatorManifest(string json)
    {
        try
        {
            var document = new System.Web.Script.Serialization.JavaScriptSerializer()
                .DeserializeObject(json) as Dictionary<string, object>;
            if (document is null || !document.TryGetValue("runtime", out var runtimeValue)
                || runtimeValue is not Dictionary<string, object> runtime
                || !runtime.TryGetValue("library_path", out var library) || library is not string path)
                return false;
            return string.Equals(Path.GetFileName(path.Replace('/', '\\')),
                "openxr_simulator.dll", StringComparison.OrdinalIgnoreCase);
        }
        catch { return false; }
    }

    private static bool UsesSimulatorRuntime()
    {
        var manifest = ActiveRuntimeManifest();
        try { return manifest is not null && IsSimulatorManifest(File.ReadAllText(manifest)); }
        catch { return false; }
    }

    private static bool UsesSteamVrRuntime()
    {
        var manifest = ActiveRuntimeManifest();
        if (manifest is null) return false;
        return manifest.IndexOf("steamvr", StringComparison.OrdinalIgnoreCase) >= 0
            || manifest.IndexOf("steamxr", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    internal static bool IsSupportedGameDirectory(string? directory) =>
        !string.IsNullOrWhiteSpace(directory)
        && File.Exists(Path.Combine(directory, "DOOMx64vk.exe"));

    public static string? FindDoomInstall()
    {
        var roots = new List<string>();
        var steam = FindSteam();
        if (!string.IsNullOrWhiteSpace(steam))
        {
            var steamRoot = Path.GetDirectoryName(steam)!;
            roots.Add(steamRoot);
            var libraryFile = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
            if (File.Exists(libraryFile))
            {
                foreach (System.Text.RegularExpressions.Match match in
                    System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(libraryFile), "\\\"path\\\"\\s+\\\"([^\\\"]+)\\\""))
                    roots.Add(match.Groups[1].Value.Replace("\\\\", "\\"));
            }
        }
        foreach (var root in roots.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            var candidate = Path.Combine(root, "steamapps", "common", "DOOM");
            if (IsSupportedGameDirectory(candidate)) return candidate;
        }
        return null;
    }

    private static string? FindSteam()
    {
        var candidates = new List<string?>();
        foreach (var (hive, view) in new[] {
            (RegistryHive.CurrentUser, RegistryView.Default),
            (RegistryHive.LocalMachine, RegistryView.Registry32),
            (RegistryHive.LocalMachine, RegistryView.Registry64)
        })
        {
            try
            {
                using var baseKey = RegistryKey.OpenBaseKey(hive, view);
                using var key = baseKey.OpenSubKey(@"SOFTWARE\Valve\Steam");
                var executable = key?.GetValue("SteamExe") as string;
                var directory = (key?.GetValue("SteamPath") ?? key?.GetValue("InstallPath")) as string;
                candidates.Add(executable);
                if (!string.IsNullOrWhiteSpace(directory)) candidates.Add(Path.Combine(directory, "steam.exe"));
            }
            catch { }
        }
        candidates.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Steam", "steam.exe"));
        candidates.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Steam", "steam.exe"));
        return candidates.Where(p => !string.IsNullOrWhiteSpace(p)).FirstOrDefault(File.Exists);
    }

    private static bool IsSteamInstall(string directory)
    {
        var normalized = Path.GetFullPath(directory).Replace('/', '\\').TrimEnd('\\');
        return normalized.IndexOf("\\steamapps\\common\\DOOM", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    private static bool IsProcessRunning(string processName)
    {
        var processes = Process.GetProcessesByName(processName);
        try { return processes.Length != 0; }
        finally { foreach (var process in processes) process.Dispose(); }
    }

    private static void EnsureNativeControllerBindings(string backWeapon)
    {
        var roots = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                "Saved Games", "id Software", "DOOM")
        };
        try
        {
            using var shellFolders = Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders");
            const string savedGamesId = "{4C5C32FF-BB9D-43B0-BFBA-6A3DCEE7D514}";
            if (shellFolders?.GetValue(savedGamesId, null,
                    RegistryValueOptions.DoNotExpandEnvironmentNames) is string redirected)
                roots.Add(Path.Combine(Environment.ExpandEnvironmentVariables(redirected),
                    "id Software", "DOOM"));
        }
        catch { }

        foreach (var root in roots.Where(Directory.Exists))
        foreach (var config in Directory.EnumerateFiles(root, "DOOMConfig.cfg", SearchOption.AllDirectories))
            PatchCampaignControllerBindings(config, backWeapon);
    }

    internal static void PatchCampaignControllerBindings(string configPath, string backWeapon)
    {
        _ = backWeapon; // Selection now uses DOOM's proven keyboard 1..8 binds.
        var lines = File.ReadAllLines(configPath).ToList();
        var changed = false;

        // Restore JOY4/JOY7/JOY8 values changed by earlier global experiments.
        // Only campaign bindset 0 belongs to this adapter. Other bindsets are
        // deliberately untouched even if a legacy backup contains them.
        var backupPath = configPath + ".kharvox-backup";
        var backupLines = File.Exists(backupPath)
            ? File.ReadAllLines(backupPath).ToList() : null;
        if (backupLines is not null)
        {
            foreach (var bindset in new[] { 0 })
            {
                var header = "bindset " + bindset;
                var sectionStart = lines.FindIndex(line => line.Trim().Equals(header, StringComparison.OrdinalIgnoreCase));
                var backupStart = backupLines.FindIndex(line => line.Trim().Equals(header, StringComparison.OrdinalIgnoreCase));
                if (sectionStart < 0 || backupStart < 0) continue;
                var sectionEnd = lines.FindIndex(sectionStart + 1,
                    line => line.TrimStart().StartsWith("bindset ", StringComparison.OrdinalIgnoreCase));
                if (sectionEnd < 0) sectionEnd = lines.Count;
                var backupEnd = backupLines.FindIndex(backupStart + 1,
                    line => line.TrimStart().StartsWith("bindset ", StringComparison.OrdinalIgnoreCase));
                if (backupEnd < 0) backupEnd = backupLines.Count;
                foreach (var rejectedBinding in new[] {
                    (Button: "JOY4", Value: "bind \"JOY4\" \"_use\""),
                    (Button: "JOY7", Value: "bind \"JOY7\" \"_supermeter\""),
                    (Button: "JOY8", Value: "bind \"JOY8\" \"_attack2 _use\"")
                })
                {
                    var prefix = "bind \"" + rejectedBinding.Button + "\"";
                    var index = lines.FindIndex(sectionStart + 1, sectionEnd - sectionStart - 1,
                        line => line.TrimStart().StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
                    var backupIndex = backupLines.FindIndex(backupStart + 1, backupEnd - backupStart - 1,
                        line => line.TrimStart().StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
                    if (index < 0 || !lines[index].Equals(rejectedBinding.Value, StringComparison.OrdinalIgnoreCase))
                        continue;
                    if (backupIndex >= 0)
                    {
                        if (lines[index] == backupLines[backupIndex]) continue;
                        lines[index] = backupLines[backupIndex];
                    }
                    else
                    {
                        // Earlier Kharvox builds may have inserted a binding
                        // which did not exist at all in the original profile.
                        lines.RemoveAt(index);
                        sectionEnd--;
                    }
                    changed = true;
                }
            }
        }

        var start = lines.FindIndex(line => line.Trim().Equals("bindset 0", StringComparison.OrdinalIgnoreCase));
        if (start >= 0)
        {
            var end = lines.FindIndex(start + 1, line => line.TrimStart().StartsWith("bindset ", StringComparison.OrdinalIgnoreCase));
            if (end < 0) end = lines.Count;
            if (backupLines is not null)
            {
                // r52-r54 tried JOY7 and D-pad channels for _weapN. DOOM
                // ignored all of them during playtesting. Restore every such
                // KHARVOX binding from the original profile; normal shoulder
                // favorites now synthesize the proven keyboard 1..8 path.
                foreach (var button in new[] { "JOY7", "JOY_DPAD_LEFT", "JOY_DPAD_DOWN" })
                {
                    var prefix = "bind \"" + button + "\"";
                    var current = lines.FindIndex(start + 1, end - start - 1,
                        line => line.TrimStart().StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
                    if (current < 0 || !IsKharvoxDirectWeaponBinding(lines[current], button))
                        continue;
                    var backupStart = backupLines.FindIndex(line => line.Trim().Equals(
                        "bindset 0", StringComparison.OrdinalIgnoreCase));
                    var backupEnd = backupStart < 0 ? -1 : backupLines.FindIndex(backupStart + 1,
                        line => line.TrimStart().StartsWith("bindset ", StringComparison.OrdinalIgnoreCase));
                    if (backupStart >= 0 && backupEnd < 0) backupEnd = backupLines.Count;
                    var backupBinding = backupStart < 0 ? -1 : backupLines.FindIndex(
                        backupStart + 1, backupEnd - backupStart - 1,
                        line => line.TrimStart().StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
                    if (backupBinding >= 0) lines[current] = backupLines[backupBinding];
                    else { lines.RemoveAt(current); end--; }
                    changed = true;
                }
            }

            var bindings = new List<(string Button, string Command)> {
                (Button: "JOY2", Command: "_crouch _menuCancel"),
                (Button: "JOY3", Command: "_quick3"),
                (Button: "JOY8", Command: "_attack2"),
                (Button: "JOY10", Command: "_inventory"),
                (Button: "JOY_DPAD_RIGHT", Command: "_use"),
                (Button: "JOY_DPAD_LEFT", Command: "_quick2")
            };
            var insertion = Math.Min(start + 2, end);
            foreach (var binding in bindings)
            {
                var prefix = "bind \"" + binding.Button + "\"";
                var index = lines.FindIndex(start + 1, end - start - 1,
                    line => line.TrimStart().StartsWith(prefix, StringComparison.OrdinalIgnoreCase));
                var expected = prefix + " \"" + binding.Command + "\"";
                if (index >= 0 && lines[index] == expected) continue;
                if (index >= 0) lines[index] = expected;
                else { lines.Insert(insertion++, expected); end++; }
                changed = true;
            }
        }

        if (!changed) return;

        var backup = configPath + ".kharvox-backup";
        if (!File.Exists(backup)) File.Copy(configPath, backup);
        File.WriteAllLines(configPath, lines, new UTF8Encoding(false));
    }

    private static bool IsKharvoxDirectWeaponBinding(string line, string button)
    {
        var trimmed = line.Trim();
        for (var weapon = 0; weapon <= 7; weapon++)
            if (trimmed.Equals($"bind \"{button}\" \"_weap{weapon}\"",
                    StringComparison.OrdinalIgnoreCase)) return true;
        return false;
    }

    internal static void WriteManifest(string manifestPath, string dllPath)
    {
        var escapedPath = dllPath.Replace("\\", "\\\\").Replace("\"", "\\\"");
        var revision = Math.Max(1, FileVersionInfo.GetVersionInfo(dllPath).FilePrivatePart).ToString(Invariant);
        var json = "{\r\n" +
            "  \"file_format_version\": \"1.2.0\",\r\n" +
            "  \"layer\": {\r\n" +
            "    \"name\": \"" + LayerName + "\",\r\n" +
            "    \"type\": \"GLOBAL\",\r\n" +
            "    \"library_path\": \"" + escapedPath + "\",\r\n" +
            "    \"api_version\": \"1.3.0\",\r\n" +
            "    \"implementation_version\": \"" + revision + "\",\r\n" +
            "    \"description\": \"KHARVOX Vulkan OpenXR layer\",\r\n" +
            "    \"enable_environment\": { \"KHARVOX_ENABLE_LAYER\": \"1\" },\r\n" +
            "    \"disable_environment\": { \"KHARVOX_DISABLE_LAYER\": \"1\" }\r\n" +
            "  }\r\n}";
        File.WriteAllText(manifestPath, json, Encoding.UTF8);
    }

    private static string ReadCalibration(string directory, string fileName, string fallback)
    {
        var path = Path.Combine(directory, fileName);
        if (!File.Exists(path)) return fallback;
        var value = File.ReadAllText(path).Trim();
        return decimal.TryParse(value, NumberStyles.Float, Invariant, out _) ? value : fallback;
    }

    internal static string ReadCalibrationWithDefault(
        string directory, string savedFileName, string defaultFileName, string builtInFallback) =>
        ReadCalibration(directory, savedFileName,
            ReadCalibration(directory, defaultFileName, builtInFallback));

    private static string Inv(decimal value) => value.ToString(Invariant);

    private static string JoinArguments(IEnumerable<string> arguments) =>
        string.Join(" ", arguments.Select(a => "\"" + a.Replace("\"", "\\\"") + "\""));
}
