using System.Reflection;

namespace KharvoxLauncher;

internal static class SelfTest
{
    private static void VerifyProcessStatusPreservesExitCode()
    {
        var flags = BindingFlags.Static | BindingFlags.NonPublic;
        var current = typeof(KharvoxRunner).GetField("currentGame", flags)!;
        var operation = typeof(KharvoxRunner).GetMethod("BeginLaunch", flags)!;
        var previous = current.GetValue(null);
        using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "cmd.exe"),
            "/d /c exit 37") { UseShellExecute = false, CreateNoWindow = true })!;
        process.WaitForExit();
        using var lease = (IDisposable)operation.Invoke(null, null)!;
        try
        {
            current.SetValue(null, process);
            Require(!KharvoxRunner.IsRunning, "exited process is not reported as running during startup");
            Require(!KharvoxRunner.IsRunning, "repeated status polling remains safe");
            Require(ReferenceEquals(current.GetValue(null), process), "status preserves startup process ownership");
            Require(process.ExitCode == 37, "startup can read original exit code after UI status polling");
        }
        finally { current.SetValue(null, previous); }
    }

    internal static int Run()
    {
        var testRoot = Path.Combine(Path.GetTempPath(), "kharvox-self-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            Directory.CreateDirectory(testRoot);
            VerifyModConflictPreflight(testRoot);
            VerifyProcessStatusPreservesExitCode();
            bool sfsBlocked=false;
            try { VulkanSfs.EnsureAvailable(testRoot); }
            catch (InvalidOperationException e) { sfsBlocked=e.Message==VulkanSfs.Blocker; }
            Require(sfsBlocked,"unverified SFS provider is blocked before launching the game");
            VerifySfsPackage(testRoot);
            Require(KharvoxRunner.IsPimaxRuntimeManifest(@"C:\Pimax\pimax-openxr.json"), "Pimax runtime blocked");
            Require(KharvoxRunner.IsPimaxRuntimeManifest(@"C:\Runtime\PiOpenXR.json"), "PiOpenXR runtime blocked case-insensitively");
            Require(!KharvoxRunner.IsPimaxRuntimeManifest(@"C:\SteamVR\steamxr_win64.json"), "SteamVR remains supported");
            Require(!KharvoxRunner.IsPimaxRuntimeManifest(null), "missing runtime is not classified as Pimax");
            Require(VrGameIntroSession.ShouldPlayIntro(false, true), "first intro required despite default disable checkbox");
            Require(!VrGameIntroSession.ShouldPlayIntro(true, true), "checked disables repeat intro");
            Require(VrGameIntroSession.ShouldPlayIntro(true, false), "unchecked repeats intro");
            var introSettingsPath = Path.Combine(testRoot, "intro-settings.json");
            LauncherSettingsStore.Save(introSettingsPath, new LauncherSettings { SettingsVersion = 31, DisableVrIntro = false });
            Require(!LauncherSettingsStore.Load(introSettingsPath).DisableVrIntro, "repeat intro preference persists");
            LauncherSettingsStore.Save(introSettingsPath, new LauncherSettings { SettingsVersion = 30, DisableVrIntro = false });
            Require(!LauncherSettingsStore.Load(introSettingsPath).DisableVrIntro, "old settings default to playing intro every launch");
            var release = new Version(0, 4, 0);
            Require(VrGameIntroSession.NeedsIntro(null, release), "first install plays intro");
            Require(VrGameIntroSession.NeedsIntro("Standalone VR intro dismissed", release), "legacy marker migrates by playing intro once");
            Require(VrGameIntroSession.NeedsIntro("0.3.0", release), "upgrade replays intro");
            Require(!VrGameIntroSession.NeedsIntro("0.4.0\r\n", release), "same release skips intro");
            Require(!VrGameIntroSession.NeedsIntro("0.5.0", release), "downgrade preserves highest seen release");
            Require(VrGameIntroSession.NeedsIntro("0.9.0", new Version(0, 10, 0)), "release comparison is numeric");
            Require(VrGameIntroSession.NeedsIntro("invalid", release), "invalid marker replays intro");
            var unseenIntroMarker = Path.Combine(testRoot, "intro-unseen");
            string? introSkipStatus = null;
            Require(VrGameIntroSession.StartAsync(testRoot, message => introSkipStatus = message,
                unseenIntroMarker).GetAwaiter().GetResult() is null,
                "missing optional intro allows DOOM startup");
            Require(!File.Exists(unseenIntroMarker) && introSkipStatus?.Contains("starting DOOM") == true,
                "missing intro reports skip without marking it as seen");
            Require(RenderMemoryCapacityException.ExitCode == 0x4b480001,
                "memory capacity exit status matches the native guard");
            Require(new RenderMemoryCapacityException().Message.Contains("Reduce Render Scale") &&
                RenderMemoryCapacityException.ExitCode != unchecked((int)0xe06d7363),
                "English capacity guidance is distinct from a generic DOOM exception");
            Require(HeadsetUnavailableException.ExitCode == 0x4b480002 && new HeadsetUnavailableException().Message.Contains("Connect your headset"), "Controlled headset startup stop has actionable English guidance");
            Require(VrGameIntroSession.StartupFailure(HeadsetUnavailableException.ExitCode) is HeadsetUnavailableException,
                "intro missing-headset status uses actionable headset guidance");
            Require(VrGameIntroSession.StartupFailure(1) is not HeadsetUnavailableException,
                "generic intro failure is not mislabeled as missing headset");
            TestLayerIsolation(testRoot);

            TestNativeLaunchRecovery(Path.Combine(testRoot, "native-recovery"));
            var validationRoot=Path.Combine(testRoot,"validation-package");
            Directory.CreateDirectory(Path.Combine(validationRoot,"validation"));
            var normalNativeStart=new System.Diagnostics.ProcessStartInfo();
            var inheritedLayers=normalNativeStart.EnvironmentVariables["VK_INSTANCE_LAYERS"];
            Require(NativeValidation.Configure(normalNativeStart,true,validationRoot) is null,"normal Native startup does not enable validation or require its DLL");
            Require(normalNativeStart.EnvironmentVariables["VK_INSTANCE_LAYERS"]==inheritedLayers,"normal Native preserves inherited layer environment");
            File.WriteAllText(Path.Combine(validationRoot,"native_validate_quality"),"");
            var validationStart=new System.Diagnostics.ProcessStartInfo();
            Require(NativeValidation.Configure(validationStart,false,validationRoot) is null,"AER ignores Native validation marker");
            foreach(var file in new[]{"VkLayer_khronos_validation.dll","VkLayer_khronos_validation.json"})
                File.WriteAllText(Path.Combine(validationRoot,"validation",file),"test placeholder");
            var validationLog=NativeValidation.Configure(validationStart,true,validationRoot);
            Require(validationLog is not null&&validationLog.StartsWith(Path.Combine(Path.GetTempPath(),"KHARVOX-NATIVE-VALIDATION-")),"Native validation log stays in TEMP");
            Require(validationStart.EnvironmentVariables["VK_INSTANCE_LAYERS"]!.Split(';').Contains("VK_LAYER_KHRONOS_validation"),"Native validation enabled only in child environment");
            var validationLayers=validationStart.EnvironmentVariables["VK_INSTANCE_LAYERS"];
            Require(NativeValidation.Configure(validationStart,true,validationRoot)!=validationLog,"retry keeps previous diagnostic log");
            Require(validationStart.EnvironmentVariables["VK_INSTANCE_LAYERS"]==validationLayers,"retry does not duplicate validation layer");
            File.WriteAllText(Path.Combine(validationRoot,NativeLaunchRecovery.RefusalFile),"diagnostic test");
            Require(NativeLaunchRecovery.DiagnosticFailure(validationRoot) is not null,"validation failure cannot silently retry AER");

            var settingsPath = Path.Combine(testRoot, "settings.json");
            var expected = new LauncherSettings(
                0, Path.Combine(testRoot, "configured-install"), false, true, true, true, "AER", 100m, true,
                0, 1, 230m, 45m, .35m, 0, 1, 2, 0, false, false, 2.8m,
                2, false, 0, false, false, true, true, 2, true, true);
            LauncherSettingsStore.Save(settingsPath, expected);
            var actual = LauncherSettingsStore.Load(settingsPath);
            Require(actual.SettingsVersion == LauncherSettings.CurrentVersion, "settings version");
            var legacyAaPath = Path.Combine(testRoot, "legacy-aa.json");
            File.WriteAllText(legacyAaPath, "{\"SettingsVersion\":29,\"DisableTssaa\":true,\"DisableAa\":true}");
            Require(!LauncherSettingsStore.Load(legacyAaPath).DisableAa, "legacy TSSAA test does not disable SMAA");

            Require(actual.GamePath == expected.GamePath, "settings path round trip");
            Require(actual.RendererMode == expected.RendererMode, "settings renderer round trip");
            Require(actual.UseFsrUpscaling, "FSR1 setting round trip");
            var nativeSettings=new LauncherSettings {RendererMode="NATIVE"};
            LauncherSettingsStore.Save(Path.Combine(testRoot,"native.json"),nativeSettings);
            Require(LauncherSettingsStore.Load(Path.Combine(testRoot,"native.json")).RendererMode=="NATIVE","native backend roundtrip");

            Require(actual.OtherCinematicsInQuad, "selective cinematic Quad round trip");
            Require(actual.CinewindowFollowsHeadset, "Cinewindow headset-follow round trip");
            Require(actual.EnableBhaptics, "bHaptics setting round trip");
            Require(actual.UsePsvr2Toolkit, "PSVR2 Toolkit setting round trip");
            Require(actual.ExtendedLogging, "extended logging setting round trip");
            Require(actual.ShowHands, "Show Hands setting round trip");
            Require(actual.HandCalibrationMode == 1,
                "unified hand calibration round trip");
            Require(actual.MovementDirection == 1,
                "movement direction setting round trip");
            Require(actual.BackWeapon == 2, "Back weapon setting round trip");
            Require(!new LauncherSettings().EnableBhaptics, "bHaptics default false");
            Require(!new LauncherSettings().UsePsvr2Toolkit,
                "PSVR2 Toolkit default false");
            Require(!new LauncherSettings().UseFsrUpscaling, "FSR1 default false");
            Require(!new LauncherSettings().ExtendedLogging,
                "extended logging default false");
            Require(new LauncherSettings().ShowHands && LauncherPresetPolicy.DefaultEnableHands,
                "Enable Hands default true for fresh settings and all presets");
            Require(new LauncherSettings().HandCalibrationMode == 0,
                "hand calibration defaults off");
            Require(new LauncherSettings().BackWeapon == 1,
                "Combat Shotgun default back weapon");
            RequireImmersivePreset(0, true, "Recommended");
            RequireImmersivePreset(2, false, "Intense");
            Require(!LauncherPresetPolicy.TryGet(1, out _)
                && !LauncherPresetPolicy.TryGet(3, out _),
                "Comfort and Custom retain their independent handling");
            VerifyHandsMainPage(testRoot);
            VerifyScrollableWindow(testRoot);
            foreach (var steam in new[]{false,true}) {
                Require(KharvoxRunner.EffectiveRenderScale(80m,steam)==80m
                    && KharvoxRunner.EffectiveRenderScale(1000m,steam)==1000m,
                    "all runtimes preserve selected source supersampling");
                Require(KharvoxRunner.SteamNativeResolutionScale(80m,steam)==100m,
                    "no second engine scale on SteamVR");
            }
            bool oversizedRejected=false;
            try { KharvoxRunner.RenderDimension(3840,decimal.MaxValue); }
            catch(InvalidOperationException) { oversizedRejected=true; }
            Require(oversizedRejected,"unrepresentable dimensions rejected before cast/multiplication");
            Require(KharvoxRunner.RenderDimension(1,250m)==3,"dimension rounding matches layer");

            var noRunAsAdmin = new DoomRunAsAdminProbe(ProbeBoolean.False, null);
            var normalPreflight = LaunchPreflight.Evaluate(
                ProbeBoolean.False, noRunAsAdmin, ProbeBoolean.False);
            Require(normalPreflight.Passed
                && normalPreflight.FailureReason == "none",
                "non-elevated launch preflight passes");

            var elevatedLauncher = LaunchPreflight.Evaluate(
                ProbeBoolean.True, noRunAsAdmin, ProbeBoolean.False);
            Require(!elevatedLauncher.Passed
                && elevatedLauncher.FailureReason == "launcher-elevated"
                && elevatedLauncher.FailureMessage!.Contains("user-specific KHARVOX layer"),
                "elevated launcher is rejected with Vulkan-layer explanation");

            var appCompatGame = Path.Combine(testRoot, "DOOMx64vk.exe");
            var hkcuRunAsAdmin = LaunchPreflight.ProbeDoomRunAsAdmin(appCompatGame,
                (hive, view, _, valueName) =>
                    hive == Microsoft.Win32.RegistryHive.CurrentUser
                    && view == Microsoft.Win32.RegistryView.Registry64
                    && valueName == appCompatGame
                        ? RegistryValueProbe.Present("RUNASADMIN")
                        : RegistryValueProbe.Missing());
            Require(hkcuRunAsAdmin.State == ProbeBoolean.True
                && hkcuRunAsAdmin.RegistryLocation!.StartsWith("HKCU "),
                "RUNASADMIN under HKCU is detected");

            var hklmRunAsAdmin = LaunchPreflight.ProbeDoomRunAsAdmin(appCompatGame,
                (hive, view, _, _) =>
                    hive == Microsoft.Win32.RegistryHive.LocalMachine
                    && view == Microsoft.Win32.RegistryView.Registry32
                        ? RegistryValueProbe.Present("WIN8 RUNASADMIN")
                        : RegistryValueProbe.Missing());
            Require(hklmRunAsAdmin.State == ProbeBoolean.True
                && hklmRunAsAdmin.RegistryLocation!.StartsWith("HKLM ")
                && hklmRunAsAdmin.RegistryLocation.Contains("Registry32"),
                "RUNASADMIN under HKLM Registry32 is detected");

            var combinedRunAsAdmin = LaunchPreflight.ProbeDoomRunAsAdmin(appCompatGame,
                (_, _, _, _) => RegistryValueProbe.Present("~ HIGHDPIAWARE RuNaSaDmIn DISABLEDXMAXIMIZEDWINDOWEDMODE"));
            Require(combinedRunAsAdmin.State == ProbeBoolean.True,
                "combined AppCompat RUNASADMIN token is detected case-insensitively");

            var missingAppCompat = LaunchPreflight.ProbeDoomRunAsAdmin(appCompatGame,
                (_, _, _, _) => RegistryValueProbe.Missing());
            Require(missingAppCompat.State == ProbeBoolean.False,
                "missing AppCompat registry keys are a clean negative result");

            var inaccessibleAppCompat = LaunchPreflight.ProbeDoomRunAsAdmin(appCompatGame,
                (_, _, _, _) => RegistryValueProbe.Inaccessible());
            var inaccessiblePreflight = LaunchPreflight.Evaluate(
                ProbeBoolean.False, inaccessibleAppCompat,
                LaunchPreflight.AggregateSteamElevations(new[] { ProbeBoolean.Unknown }));
            Require(inaccessibleAppCompat.State == ProbeBoolean.Unknown
                && inaccessiblePreflight.SteamElevation == ProbeBoolean.Unknown
                && inaccessiblePreflight.Passed,
                "denied registry and Steam process access remain unknown and non-blocking");

            var appCompatFailure = LaunchPreflight.Evaluate(
                ProbeBoolean.False, hkcuRunAsAdmin, ProbeBoolean.False);
            Require(!appCompatFailure.Passed
                && appCompatFailure.FailureReason == "doom-runasadmin-compatibility"
                && appCompatFailure.FailureMessage!.Contains(hkcuRunAsAdmin.RegistryLocation!),
                "DOOM compatibility elevation is rejected with registry location");
            var steamFailure = LaunchPreflight.Evaluate(
                ProbeBoolean.False, noRunAsAdmin, ProbeBoolean.True);
            Require(!steamFailure.Passed
                && steamFailure.FailureReason == "steam-elevated",
                "proven elevated Steam process is rejected");
            Require(normalPreflight.StructuredLogEntry().Contains("launcherElevated=false")
                && normalPreflight.StructuredLogEntry().Contains("steamElevation=false")
                && normalPreflight.StructuredLogEntry().Contains("preflightResult=passed"),
                "structured preflight logging fields");

            const string startupFailure = "DOOM startup timed out.";
            var loadedLayerMessage = VulkanLoaderStartupDiagnosis.AddToFailureMessage(
                startupFailure,
                "Loading layer manifest KharvoxLayer.json for VK_LAYER_KHARVOX_OPENXR",
                false);
            Require(loadedLayerMessage == startupFailure,
                "loader log with KHARVOX layer keeps the original timeout diagnosis");
            var missingLayerMessage = VulkanLoaderStartupDiagnosis.AddToFailureMessage(
                startupFailure, "Vulkan loader: no matching implicit layers", false);
            Require(missingLayerMessage.Contains("KHARVOX layer was not loaded")
                && missingLayerMessage.Contains("possible cause"),
                "loader log without KHARVOX layer reports the missing layer cautiously");
            var provenElevationMessage = VulkanLoaderStartupDiagnosis.AddToFailureMessage(
                startupFailure, string.Empty, true);
            Require(provenElevationMessage.Contains("confirmed elevated process rights"),
                "loader diagnosis only asserts elevation when preflight proved it");

            var legacyBackWeaponPath = Path.Combine(testRoot, "settings-v18-back-weapon.json");
            File.WriteAllText(legacyBackWeaponPath,
                "{\"SettingsVersion\":18,\"BackWeapon\":7}");
            Require(LauncherSettingsStore.Load(legacyBackWeaponPath).BackWeapon == 1,
                "legacy Back weapon migration uses Combat Shotgun");

            var retiredBodyPath = Path.Combine(testRoot, "settings-v19-retired-body.json");
            File.WriteAllText(retiredBodyPath,
                "{\"SettingsVersion\":19,\"ShowBody\":true,\"BackWeapon\":2}");
            var retiredBodySettings = LauncherSettingsStore.Load(retiredBodyPath);
            Require(retiredBodySettings.BackWeapon == 2,
                "retired Show Body field is ignored without losing settings");
            LauncherSettingsStore.Save(retiredBodyPath, retiredBodySettings);
            Require(!File.ReadAllText(retiredBodyPath).Contains("ShowBody"),
                "retired Show Body field is removed on save");

            var oldSettingsPath = Path.Combine(testRoot, "settings-v16.json");
            File.WriteAllText(oldSettingsPath, "{\"SettingsVersion\":16,\"EnableBhaptics\":true}");
            var oldSettings = LauncherSettingsStore.Load(oldSettingsPath);
            Require(oldSettings.SettingsVersion == 16 && !oldSettings.EnableBhaptics,
                "legacy bHaptics migration remains opt-in");
            File.WriteAllText(oldSettingsPath, "{\"SettingsVersion\":16}");
            oldSettings = LauncherSettingsStore.Load(oldSettingsPath);
            Require(!oldSettings.EnableBhaptics, "legacy missing bHaptics defaults false");

            var oldPsvr2Path = Path.Combine(testRoot, "settings-v20-psvr2.json");
            File.WriteAllText(oldPsvr2Path,
                "{\"SettingsVersion\":20,\"UsePsvr2Toolkit\":true}");
            Require(!LauncherSettingsStore.Load(oldPsvr2Path).UsePsvr2Toolkit,
                "legacy PSVR2 Toolkit migration remains opt-in");

            var oldFsrPath = Path.Combine(testRoot, "settings-v21-fsr.json");
            File.WriteAllText(oldFsrPath,
                "{\"SettingsVersion\":21,\"UseFsrUpscaling\":true}");
            Require(!LauncherSettingsStore.Load(oldFsrPath).UseFsrUpscaling,
                "legacy FSR1 migration remains opt-in");

            var oldMovementPath = Path.Combine(testRoot, "settings-v23-movement.json");
            File.WriteAllText(oldMovementPath,
                "{\"SettingsVersion\":23,\"MovementDirection\":1}");
            Require(LauncherSettingsStore.Load(oldMovementPath).MovementDirection == 0,
                "legacy settings migrate to Head direction");

            var oldLoggingPath = Path.Combine(testRoot, "settings-v24-logging.json");
            File.WriteAllText(oldLoggingPath,
                "{\"SettingsVersion\":24,\"ExtendedLogging\":true}");
            Require(!LauncherSettingsStore.Load(oldLoggingPath).ExtendedLogging,
                "legacy settings migrate to disabled extended logging");

            var oldHandsPath = Path.Combine(testRoot, "settings-v25-hands.json");
            File.WriteAllText(oldHandsPath,
                "{\"SettingsVersion\":25,\"ShowHands\":true}");
            Require(LauncherSettingsStore.Load(oldHandsPath).ShowHands,
                "legacy settings migrate to default-on Enable Hands");
            foreach (var oldVersion in new[] { 26, 27, 28 })
            {
                File.WriteAllText(oldHandsPath,
                    "{\"SettingsVersion\":" + oldVersion + ",\"ShowHands\":false}");
                Require(LauncherSettingsStore.Load(oldHandsPath).ShowHands,
                    "developer-era hands setting migrates to enabled");
            }
            actual.ShowHands = false;
            actual.HandCalibrationMode = 0;
            LauncherSettingsStore.Save(settingsPath, actual);
            Require(!LauncherSettingsStore.Load(settingsPath).ShowHands,
                "current-schema explicit disabled hands survive roundtrip");

            var oldHandCalibrationPath = Path.Combine(testRoot,
                "settings-v26-hand-calibration.json");
            File.WriteAllText(oldHandCalibrationPath,
                "{\"SettingsVersion\":26,\"HandCalibrationMode\":2}");
            Require(LauncherSettingsStore.Load(oldHandCalibrationPath)
                    .HandCalibrationMode == 0,
                "legacy settings migrate to disabled hand calibration");
            foreach (var previousMode in new[] { 0, 1, 2 })
            {
                File.WriteAllText(oldHandCalibrationPath,
                    "{\"SettingsVersion\":27,\"HandCalibrationMode\":" + previousMode + "}");
                Require(LauncherSettingsStore.Load(oldHandCalibrationPath)
                        .HandCalibrationMode == (previousMode == 0 ? 0 : 1),
                    "rotation/position migrate to unified hand checkbox");
            }

            var disabledPreflight = BhapticsBridgeSession.Inspect(false, testRoot);
            Require(disabledPreflight.State == BhapticsPreflightState.Disabled
                && !disabledPreflight.CanStart, "disabled bHaptics preflight");
            var disabledStatusCalled = false;
            Require(BhapticsBridgeSession.TryStart(false, testRoot,
                    _ => disabledStatusCalled = true) is null
                && !disabledStatusCalled, "disabled bHaptics starts no process");
            var processInfo = new System.Diagnostics.ProcessStartInfo();
            BhapticsBridgeSession.ApplyToGame(processInfo, null);
            Require(!processInfo.EnvironmentVariables.ContainsKey("KHARVOX_BHAPTICS_PIPE_NAME")
                && !processInfo.EnvironmentVariables.ContainsKey("KHARVOX_BHAPTICS_SESSION_TOKEN"),
                "disabled bHaptics sets no IPC environment");
            var missingBridge = BhapticsBridgeSession.Inspect(true, testRoot);
            Require(missingBridge.State == BhapticsPreflightState.MissingBridge
                && !missingBridge.CanStart, "missing bridge degrades safely");
            File.WriteAllBytes(Path.Combine(testRoot, "KharvoxBhapticsBridge.exe"),
                Array.Empty<byte>());
            var sharedBhaptics = Path.Combine(testRoot, "shared-sdk");
            Directory.CreateDirectory(sharedBhaptics);
            Require(!BhapticsBridgeSession.Inspect(true, testRoot, sharedBhaptics).CanStart,
                "missing release and shared SDK degrades safely");
            File.WriteAllBytes(Path.Combine(sharedBhaptics, "bhaptics_library.dll"), Array.Empty<byte>());
            Require(BhapticsBridgeSession.Inspect(true, testRoot, sharedBhaptics).CanStart
                && BhapticsBridgeSession.FindSdkDirectory(testRoot, sharedBhaptics) == sharedBhaptics,
                "shared user SDK survives a release-folder change");
            File.WriteAllBytes(Path.Combine(testRoot, "bhaptics_library.dll"),
                Array.Empty<byte>());
            Require(BhapticsBridgeSession.FindSdkDirectory(testRoot, sharedBhaptics) == testRoot,
                "release-local SDK takes precedence over shared SDK");
            string? localPlaybackStatus = null;
            Require(BhapticsBridgeSession.TryStart(true, testRoot,
                    message => localPlaybackStatus = message,
                    string.Empty, string.Empty) is null
                && !string.IsNullOrWhiteSpace(localPlaybackStatus),
                "credential-free local playback reaches bridge startup");
            string? faultyBridgeStatus = null;
            Require(BhapticsBridgeSession.TryStart(true, testRoot,
                    message => faultyBridgeStatus = message,
                    "test-app-id", "test-api-key") is null
                && !string.IsNullOrWhiteSpace(faultyBridgeStatus),
                "faulty enabled bridge degrades without blocking launch");
            Require(BhapticsBridgeSession.StartupSettleDelay(false, true)
                    == TimeSpan.FromSeconds(2),
                "cold bHaptics Player receives a bounded VR startup settle delay");
            Require(BhapticsBridgeSession.StartupSettleDelay(true, true)
                    == TimeSpan.Zero
                && BhapticsBridgeSession.StartupSettleDelay(false, false)
                    == TimeSpan.Zero,
                "warm or unavailable bHaptics Player does not delay VR startup");

            var psvr2Disabled = Psvr2BridgeSession.Inspect(false, testRoot);
            Require(psvr2Disabled.State == Psvr2PreflightState.Disabled
                && !psvr2Disabled.CanStart, "disabled PSVR2 preflight");
            var psvr2Info = new System.Diagnostics.ProcessStartInfo();
            Psvr2BridgeSession.ApplyToGame(psvr2Info, null);
            Require(!psvr2Info.EnvironmentVariables.ContainsKey(
                    "KHARVOX_USE_PSVR2_TOOLKIT")
                && !psvr2Info.EnvironmentVariables.ContainsKey(
                    "KHARVOX_PSVR2_PIPE_NAME"),
                "disabled PSVR2 sets no game environment");
            Require(Psvr2BridgeSession.Inspect(true, testRoot).State
                == Psvr2PreflightState.MissingBridge,
                "missing PSVR2 bridge degrades safely");
            File.WriteAllBytes(Path.Combine(testRoot, "KharvoxPsvr2Bridge.exe"),
                Array.Empty<byte>());
            Require(Psvr2BridgeSession.Inspect(true, testRoot).State
                == Psvr2PreflightState.MissingLoader,
                "missing official PSVR2 loader degrades safely");
            File.WriteAllBytes(Path.Combine(testRoot,
                "psvr2_toolkit_capi_loader.dll"), Array.Empty<byte>());
            string? faultyPsvr2Status = null;
            Require(Psvr2BridgeSession.TryStart(true, testRoot,
                    message => faultyPsvr2Status = message) is null
                && !string.IsNullOrWhiteSpace(faultyPsvr2Status),
                "bridge crash or failure does not block game start");

            var diagnosticOptions = new KharvoxLaunchOptions(
                false, true, false, false, "AER", 100m, true, testRoot,
                "Smooth", "off-hand", 230m, 45m, .35m, false, "shotgun", "barrel",
                false, false, 2.8m, "both", false, "buttons",
                false, true, true, true, "position", false, false,
                "plasma_rifle");
            Require(!diagnosticOptions.HudDebugging,
                "hand calibration exclusively owns keypad instead of HUD calibration");
            Require(diagnosticOptions.DiagnosticSummary().Contains("bHaptics=disabled"),
                "bHaptics diagnostic summary");
            Require(diagnosticOptions.DiagnosticSummary().Contains("fsr1=enabled"),
                "FSR1 diagnostic summary");
            Require(diagnosticOptions.DiagnosticSummary().Contains(
                "psvr2Toolkit=disabled"), "PSVR2 diagnostic summary");
            Require(diagnosticOptions.DiagnosticSummary().Contains("backWeapon=plasma_rifle"),
                "Back weapon diagnostic summary");
            Require(diagnosticOptions.DiagnosticSummary().Contains(
                "movementDirection=off-hand"), "movement direction diagnostic summary");
            Require(diagnosticOptions.DiagnosticSummary().Contains(
                "extendedLogging=enabled"), "extended logging diagnostic summary");
            Require(diagnosticOptions.DiagnosticSummary().Contains(
                "showHands=enabled"), "Show Hands diagnostic summary");
            Require(diagnosticOptions.DiagnosticSummary().Contains(
                "handCalibration=position"),
                "hand calibration diagnostic summary");
            var steamVrConfig = Path.Combine(testRoot, "Steam", "config", "steamvr.vrsettings");
            Directory.CreateDirectory(Path.GetDirectoryName(steamVrConfig)!);
            File.WriteAllText(steamVrConfig,
                "{\r\n" +
                "   \"steamvr\" : {\r\n" +
                "      \"disableAsync\" : true,\r\n" +
                "      \"motionSmoothing\" : false\r\n" +
                "   },\r\n" +
                "   \"steam.app.877200\" : {\r\n" +
                "      \"disableAsync\" : false\r\n" +
                "   }\r\n" +
                "}\r\n");
            Require(SteamVrSettings.EnsureKharvoxAsyncReprojection(steamVrConfig)
                    == SteamVrSettingsUpdate.AddedApplication,
                "SteamVR KHARVOX application override is added");
            var patchedSteamVrConfig = File.ReadAllText(steamVrConfig);
            Require(patchedSteamVrConfig.Contains("\"steam.app.379720\""),
                "SteamVR KHARVOX application key");
            Require(patchedSteamVrConfig.Contains("\"motionSmoothing\" : false"),
                "SteamVR unrelated global setting preserved");
            Require(patchedSteamVrConfig.Contains("\"steam.app.877200\""),
                "SteamVR unrelated application preserved");
            Require(File.Exists(steamVrConfig + ".kharvox-backup"),
                "SteamVR settings backup preserved");
            Require(SteamVrSettings.EnsureKharvoxAsyncReprojection(steamVrConfig)
                    == SteamVrSettingsUpdate.AlreadyConfigured,
                "SteamVR KHARVOX override is idempotent");

            var existingSteamVrConfig = Path.Combine(testRoot, "existing-steamvr.vrsettings");
            File.WriteAllText(existingSteamVrConfig,
                "{\n  \"steam.app.379720\": {\n    \"resolutionScale\": 125,\n    \"disableAsync\": true\n  }\n}\n");
            Require(SteamVrSettings.EnsureKharvoxAsyncReprojection(existingSteamVrConfig)
                    == SteamVrSettingsUpdate.UpdatedApplication,
                "SteamVR existing KHARVOX override is updated");
            var existingSteamVrPatched = File.ReadAllText(existingSteamVrConfig);
            Require(existingSteamVrPatched.Contains("\"resolutionScale\": 125"),
                "SteamVR existing KHARVOX scale preserved");
            Require(existingSteamVrPatched.Contains("\"disableAsync\": false"),
                "SteamVR existing KHARVOX async reprojection enabled");

            var controllerConfig = Path.Combine(testRoot, "DOOMConfig.cfg");
            File.WriteAllText(controllerConfig,
                "bindset 0\n{\n" +
                "bind \"JOY7\" \"_sprint\"\n" +
                "bind \"JOY_DPAD_LEFT\" \"_quick0\"\n" +
                "bind \"JOY_DPAD_DOWN\" \"_objectives\"\n}\n" +
                "bindset 1\n{\n" +
                "bind \"JOY7\" \"_bindset1_original\"\n" +
                "bind \"JOY_DPAD_LEFT\" \"_bindset1_left\"\n" +
                "bind \"JOY_DPAD_DOWN\" \"_bindset1_down\"\n}\n");
            KharvoxRunner.PatchCampaignControllerBindings(controllerConfig, "plasma_rifle");
            var patchedControllerConfig = File.ReadAllText(controllerConfig);
            Require(patchedControllerConfig.Contains("bind \"JOY7\" \"_sprint\""),
                "obsolete JOY7 Back weapon channel remains original");
            Require(patchedControllerConfig.Contains("bind \"JOY_DPAD_LEFT\" \"_quick2\""),
                "bindset 0 D-pad Left selects next equipment");
            Require(patchedControllerConfig.Contains("bind \"JOY_DPAD_DOWN\" \"_objectives\""),
                "bindset 0 D-pad Down remains original");
            Require(patchedControllerConfig.Contains("bind \"JOY7\" \"_bindset1_original\""),
                "other bindset JOY7 remains unchanged");
            Require(patchedControllerConfig.Contains("bind \"JOY_DPAD_LEFT\" \"_bindset1_left\""),
                "other bindset D-pad remains unchanged");
            Require(patchedControllerConfig.Contains("bind \"JOY_DPAD_DOWN\" \"_bindset1_down\""),
                "other bindset fallback D-pad remains unchanged");
            Require(File.Exists(controllerConfig + ".kharvox-backup"),
                "controller binding backup preserved");
            KharvoxRunner.PatchCampaignControllerBindings(controllerConfig, "bfg");
            var bfgControllerConfig = File.ReadAllText(controllerConfig);
            Require(bfgControllerConfig.Contains("bind \"JOY7\" \"_sprint\""),
                "native-pulse Back weapon restores original JOY7 binding");
            Require(bfgControllerConfig.Contains("bind \"JOY_DPAD_LEFT\" \"_quick2\""),
                "BFG selection preserves next equipment binding");
            Require(bfgControllerConfig.Contains("bind \"JOY_DPAD_DOWN\" \"_objectives\""),
                "native-pulse Back weapon restores original D-pad Down binding");

            Require(KharvoxRunner.ReadCalibrationWithDefault(testRoot,
                "pose_saved.cfg", "pose_default.cfg", "-8") == "-8",
                "built-in pose fallback");
            File.WriteAllText(Path.Combine(testRoot, "pose_default.cfg"), "-8\n");
            Require(KharvoxRunner.ReadCalibrationWithDefault(testRoot,
                "pose_saved.cfg", "pose_default.cfg", "25") == "-8",
                "project pose default");
            File.WriteAllText(Path.Combine(testRoot, "pose_saved.cfg"), "-7\n");
            Require(KharvoxRunner.ReadCalibrationWithDefault(testRoot,
                "pose_saved.cfg", "pose_default.cfg", "25") == "-7",
                "saved pose override");
            File.WriteAllText(Path.Combine(testRoot, "pose_saved.cfg"), "invalid\n");
            Require(KharvoxRunner.ReadCalibrationWithDefault(testRoot,
                "pose_saved.cfg", "pose_default.cfg", "25") == "-8",
                "invalid saved pose falls back to project default");

            var missingInstall = Path.Combine(testRoot, "missing-install");
            Directory.CreateDirectory(missingInstall);
            Require(!KharvoxRunner.IsSupportedGameDirectory(missingInstall), "missing installation rejection");

            var detectedInstall = Path.Combine(testRoot, "detected-install");
            Directory.CreateDirectory(detectedInstall);
            File.WriteAllBytes(Path.Combine(detectedInstall, "DOOMx64vk.exe"), Array.Empty<byte>());
            Require(KharvoxRunner.IsSupportedGameDirectory(detectedInstall), "installation detection");

            using (var firstLaunchGate = KharvoxRunner.AcquireLaunchGate())
            {
                var concurrentLaunchRejected = false;
                try
                {
                    using var ignoredSecondGate = KharvoxRunner.AcquireLaunchGate();
                }
                catch (InvalidOperationException)
                {
                    concurrentLaunchRejected = true;
                }
                Require(concurrentLaunchRejected, "concurrent game launch rejection");
            }
            using (KharvoxRunner.AcquireLaunchGate()) { }

            using var logo = Assembly.GetExecutingAssembly().GetManifestResourceStream("Kharvox.Branding.Logo");
            Require(logo is not null && logo.Length > 0, "embedded KHARVOX logo");
            using var bloodArtwork = Assembly.GetExecutingAssembly()
                .GetManifestResourceStream("Kharvox.Branding.BloodSplatter");
            Require(bloodArtwork is not null && bloodArtwork.Length > 0,
                "embedded launcher blood artwork");
            using var manualBook = Assembly.GetExecutingAssembly()
                .GetManifestResourceStream("Kharvox.Branding.ManualBook");
            Require(manualBook is not null && manualBook.Length > 0,
                "embedded manual book artwork");
            using var manualMark = Assembly.GetExecutingAssembly()
                .GetManifestResourceStream("Kharvox.Branding.ManualMark");
            Require(manualMark is not null && manualMark.Length > 0,
                "embedded transparent KHARVOX manual mark");
            using var bhapticsLogo = Assembly.GetExecutingAssembly()
                .GetManifestResourceStream("Kharvox.Branding.BhapticsLogo");
            Require(bhapticsLogo is not null && bhapticsLogo.Length > 0,
                "embedded transparent bHaptics logo");
            using var psvr2ToolkitLogo = Assembly.GetExecutingAssembly()
                .GetManifestResourceStream("Kharvox.Branding.Psvr2ToolkitLogo");
            Require(psvr2ToolkitLogo is not null && psvr2ToolkitLogo.Length > 0,
                "embedded PSVR2 Toolkit logo");
            using var controllerMapping = Assembly.GetExecutingAssembly()
                .GetManifestResourceStream("Kharvox.Branding.ControllerMapping");
            Require(controllerMapping is not null && controllerMapping.Length > 0,
                "embedded transparent Quest controller mapping");

            Require(Assembly.GetExecutingAssembly().GetManifestResourceNames()
                .Count(name => name.StartsWith("Kharvox.Licenses.", StringComparison.Ordinal)) == 11,
                "all distribution notices are embedded");
            var notices = InfoForm.LicenseNotices();
            Require(notices.Contains("Apache License") && notices.Contains("Advanced Micro Devices")
                && notices.Contains("Baldur Karlsson") && notices.Contains("dbkni")
                && notices.Contains("rombankzero"),
                "complete readable third-party license viewer payload");
            var manualPages = InfoForm.PageTitlesForTest();
            Require(manualPages.Length == 9 && manualPages[0] == "Contents",
                "manual starts with a nine-page contents page");
            Require(manualPages.Skip(1).SequenceEqual(new[]
            {
                "General Information and Limitations", "Controller Bindings", "Movement",
                "Game Options", "Rendering", "bHaptics", "PSVR2 Toolkit", "About"
            }), "manual chapter order");
            var manualLinks = InfoForm.ExternalLinksForTest();
            Require(manualLinks.All(link =>
                    Uri.TryCreate(link, UriKind.Absolute, out var uri)
                    && uri.Scheme == Uri.UriSchemeHttps),
                "manual external links are absolute HTTPS URLs");
            Require(manualLinks.Contains("https://discord.com/invite/ZFSCSDe"),
                "manual Flat2VR support invite");
            Require(manualLinks.Contains("https://discord.gg/jym8CgJPF3"),
                "manual PSVR2 Toolkit Discord support invite");

            var assemblyName = Assembly.GetExecutingAssembly().GetName().Name;
            Require(string.Equals(assemblyName, "KharvoxLauncher", StringComparison.Ordinal), "assembly name");
            return 0;
        }
        catch (Exception error)
        {
            File.WriteAllText(Path.Combine(Path.GetTempPath(), "KHARVOX-launcher-self-test-error.log"), error.ToString());
            return 1;
        }
        finally
        {
            try { if (Directory.Exists(testRoot)) Directory.Delete(testRoot, true); }
            catch { }
        }
    }

    private static void TestLayerIsolation(string directory)
    {
        var manifest = Path.Combine(directory, "KharvoxLayer.json");
        KharvoxRunner.WriteManifest(manifest, Assembly.GetExecutingAssembly().Location);
        var json = new System.Web.Script.Serialization.JavaScriptSerializer()
            .Deserialize<Dictionary<string, object>>(File.ReadAllText(manifest));
        var layer = (Dictionary<string, object>)json["layer"];
        Require(((Dictionary<string, object>)layer["enable_environment"])["KHARVOX_ENABLE_LAYER"].Equals("1"),
            "implicit layer requires process-local opt-in");
        Require(layer.ContainsKey("disable_environment"), "implicit layer retains disable switch");
        var previousEnable = Environment.GetEnvironmentVariable("KHARVOX_ENABLE_LAYER");
        var game = new System.Diagnostics.ProcessStartInfo();
        game.EnvironmentVariables["KHARVOX_DISABLE_LAYER"] = "1";
        KharvoxRunner.EnableLayerForGame(game);
        Require(game.EnvironmentVariables["KHARVOX_ENABLE_LAYER"] == "1"
            && game.EnvironmentVariables["KHARVOX_DISABLE_LAYER"] is null,
            "DOOM receives layer opt-in without inherited disable override");
        Require(Environment.GetEnvironmentVariable("KHARVOX_ENABLE_LAYER") == previousEnable,
            "layer opt-in does not modify launcher environment");

        var testKey = @"Software\KHARVOX\SelfTest\" + Guid.NewGuid().ToString("N");
        using var hive = Microsoft.Win32.RegistryKey.OpenBaseKey(
            Microsoft.Win32.RegistryHive.CurrentUser, Microsoft.Win32.RegistryView.Registry64);
        try
        {
            using var key = hive.CreateSubKey(testKey, true)!;
            var oldManifest = Path.Combine(directory, "old-release", "KharvoxLayer.json");
            key.SetValue(manifest, 0, Microsoft.Win32.RegistryValueKind.DWord);
            key.SetValue(oldManifest, 0, Microsoft.Win32.RegistryValueKind.DWord);
            key.SetValue("OtherLayer.json", 0, Microsoft.Win32.RegistryValueKind.DWord);
            KharvoxRunner.DisableKharvoxLayerRegistrations(key, manifest);
            Require((int)key.GetValue(manifest)! == 0 && (int)key.GetValue(oldManifest)! == 1,
                "startup disables stale registrations while preserving active manifest");
            KharvoxRunner.DisableKharvoxLayerRegistrations(key, string.Empty);
            Require((int)key.GetValue(manifest)! == 1 && (int)key.GetValue("OtherLayer.json")! == 0,
                "exit disables KHARVOX registration without changing other layers");
        }
        finally { hive.DeleteSubKeyTree(testKey, false); }
    }

    private static void TestNativeLaunchRecovery(string directory)
    {
        Directory.CreateDirectory(directory);
        var refusal = Path.Combine(directory, NativeLaunchRecovery.RefusalFile);
        Require(NativeLaunchRecovery.Prepare(directory, "NATIVE", directory) is null, "fresh Native start");
        File.WriteAllText(refusal, "previous recording refusal");
        Require(NativeLaunchRecovery.DiagnosticFailure(directory) is null, "ordinary Native retains recovery");
        var diagnosticMarker = Path.Combine(directory, "native_test_left_eye_only");
        File.WriteAllText(diagnosticMarker, "");
        var diagnosticFailure = NativeLaunchRecovery.DiagnosticFailure(directory);
        Require(diagnosticFailure is not null && diagnosticFailure.Contains("previous recording refusal")
            && diagnosticFailure.Contains("AER was not started") && File.Exists(refusal),
            "diagnostic stops with failure evidence intact");
        File.Delete(refusal);
        Require(NativeLaunchRecovery.DiagnosticFailure(directory) is null, "healthy diagnostic is not blocked");
        File.WriteAllText(refusal, "previous recording refusal");
        File.Delete(diagnosticMarker);
        var gpuMarker = Path.Combine(directory, "profile_native_gpu_passes");
        File.WriteAllText(gpuMarker, "");
        Require(NativeLaunchRecovery.DiagnosticFailure(directory)?.Contains("AER was not started") == true,
            "full-stereo GPU measurement cannot fall back to AER");
        File.Delete(refusal);
        Require(NativeLaunchRecovery.DiagnosticFailure(directory) is null, "healthy GPU diagnostic starts");
        File.WriteAllText(refusal, "previous recording refusal");
        File.Delete(gpuMarker);
        var frameMarker = Path.Combine(directory, "debug_native_frame_analysis");
        File.WriteAllText(frameMarker, "");
        Require(NativeLaunchRecovery.DiagnosticFailure(directory)?.Contains("AER was not started") == true,
            "hotkey frame analysis cannot silently fall back to AER");
        File.Delete(frameMarker);
        var log = Path.Combine(directory, "KHARVOX-NATIVE-STEREO.log");
        File.WriteAllText(log, "previous diagnostic evidence");
        foreach (var renderer in new[] { "AER", "AFW" })
            Require(NativeLaunchRecovery.Prepare(directory, renderer, directory) is null && File.Exists(refusal),
                "other renderer preserves Native refusal");
        // Failure to preserve evidence must not silently clear the latch.
        using (var lockedLog = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var failed = false;
            try { NativeLaunchRecovery.Prepare(directory, "NATIVE", directory); }
            catch (IOException) { failed = true; }
            Require(failed && File.Exists(refusal), "archive failure retains refusal");
        }
        var archive = NativeLaunchRecovery.Prepare(directory, "NATIVE", directory);
        Require(archive is not null && !File.Exists(refusal), "new explicit Native launch retries");
        Require(File.ReadAllText(Path.Combine(archive!, NativeLaunchRecovery.RefusalFile)) == "previous recording refusal",
            "refusal evidence archived intact");
        Require(File.ReadAllText(Path.Combine(archive!, "KHARVOX-NATIVE-STEREO.log")) == "previous diagnostic evidence"
            && File.ReadAllText(log) == "previous diagnostic evidence", "Native log preserved");
        Require(NativeLaunchRecovery.Prepare(directory, "NATIVE", directory) is null, "no duplicate archive without refusal");
        File.WriteAllText(refusal, "new recording refusal");
        Require(File.Exists(refusal), "new failure remains available to automatic AER retry");
        var secondArchive = NativeLaunchRecovery.Prepare(directory, "NATIVE", directory);
        Require(secondArchive is not null && secondArchive != archive
            && File.ReadAllText(Path.Combine(secondArchive!, NativeLaunchRecovery.RefusalFile)) == "new recording refusal"
            && File.ReadAllText(Path.Combine(archive!, NativeLaunchRecovery.RefusalFile)) == "previous recording refusal",
            "later explicit launch preserves both refusal generations");
    }

    private static void VerifyModConflictPreflight(string testRoot)
    {
        var gameDirectory = Path.Combine(testRoot, "mod-preflight");
        Directory.CreateDirectory(gameDirectory);
        foreach (var name in new[] { "DOOMx64vk.exe", "bink2w64.dll", "steam_api64.dll",
            "CChromaEditorLibrary.dll", "openvr_api.dll", "dinput8.dll.bak", "dxgi_.dll",
            "RealVR64.log", "mods.zip" })
            File.WriteAllText(Path.Combine(gameDirectory, name), "test data");
        Directory.CreateDirectory(Path.Combine(gameDirectory, "RealRepo_"));
        var backup = Path.Combine(gameDirectory, "backup");
        Directory.CreateDirectory(backup);
        File.WriteAllText(Path.Combine(backup, "dinput8.dll"), "archived");
        Require(ModConflictPreflight.FindConflicts(gameDirectory).Length == 0,
            "normal game DLLs and inactive backups do not block launch");
        foreach (var name in new[] { "DINPUT8.DLL", "dxgi.dll", "RealVR64.dll", "RealVR.ini",
            "RealConfig.bat", "version.dll", "example.ASI" })
        {
            var file = Path.Combine(gameDirectory, name);
            File.WriteAllText(file, "conflicting mod");
            Require(ModConflictPreflight.FindConflicts(gameDirectory).SequenceEqual(new[] { name }),
                "detect mod without case sensitivity: " + name);
            File.Delete(file);
        }
        Directory.CreateDirectory(Path.Combine(gameDirectory, "RealRepo"));
        File.WriteAllText(Path.Combine(gameDirectory, "dinput8.dll"), "do not modify");
        var options = new KharvoxLaunchOptions(false, true, false, false, "AER", 100m, false,
            gameDirectory, "Smooth", "head", 230m, 45m, .35m, false, "shotgun", "barrel",
            false, false, 2.8m, "both", false, "buttons", false, false, false, true,
            "off", false, false, "shotgun");
        var callbackCalled = false;
        try
        {
            KharvoxRunner.LaunchAsync(options, () => callbackCalled = true,
                _ => callbackCalled = true).GetAwaiter().GetResult();
            throw new InvalidOperationException("Conflicting mods must block the real launch path");
        }
        catch (OtherModsDetectedException error)
        {
            Require(error.Message.StartsWith("Other mods detected, please remove other mods to make Doom work."),
                "requested mod conflict message");
            Require(error.Message.Contains("dinput8.dll") && error.Message.Contains("RealRepo")
                && error.Message.Contains(gameDirectory), "error identifies every detected mod and directory");
        }
        Require(!callbackCalled, "preflight blocks before intro, bridges and game callbacks");
        Require(File.ReadAllText(Path.Combine(gameDirectory, "dinput8.dll")) == "do not modify",
            "preflight never removes or edits other mods");
        File.Delete(Path.Combine(gameDirectory, "dinput8.dll"));
        Directory.Delete(Path.Combine(gameDirectory, "RealRepo"));
        ModConflictPreflight.EnsureClean(gameDirectory);
        try
        {
            ModConflictPreflight.EnsureClean(Path.Combine(gameDirectory, "missing"));
            throw new Exception("Unreadable directory must not count as a clean scan");
        }
        catch (InvalidOperationException error)
        {
            Require(error.Message.StartsWith("Unable to check"), "failed inspection blocks safely");
        }
    }

    private static void VerifyScrollableWindow(string testRoot)
    {
        foreach (var scale in new[] { 1f, 1.5f, 2f })
        {
            using var form = new MainForm(Path.Combine(testRoot, "scroll-settings.json"));
            form.Scale(new System.Drawing.SizeF(scale, scale));
            var work = new System.Drawing.Rectangle(-1280, 0, 1280, 680);
            form.FitWorkingArea(work);
            form.PerformLayout();
            Require(work.Contains(form.Bounds), "launcher fits monitor work area at scaled DPI");
            var viewport = (Panel)form.Controls[0];
            Require(viewport.AutoScroll && viewport.Controls[0].Height > viewport.ClientSize.Height,
                "short desktop preserves scrollable content");
            Require(viewport.Controls[0].Width <= viewport.ClientSize.Width,
                "vertical scrollbar does not require horizontal scrolling");
            var tiny = new System.Drawing.Rectangle(0, 0, 480, 600);
            form.FitWorkingArea(tiny);
            Require(tiny.Contains(form.Bounds), "very narrow desktop remains reachable");
        }
    }

    private static void VerifyHandsMainPage(string testRoot)
    {
        using var form = new MainForm(Path.Combine(testRoot, "ui-settings.json"));
        T Field<T>(string name) => (T)typeof(MainForm)
            .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(form)!;
        var hands = Field<CheckBox>("showHands");
        var laser = Field<CheckBox>("laserSight");
        var captureEyes = Field<CheckBox>("captureEyes");
        Require(!captureEyes.Checked,"eye capture defaults off");
        captureEyes.Checked=true;
        Require(form.CreateLaunchOptions().CaptureEyes,"eye capture launch option");
        var disableAa = Field<CheckBox>("disableAa");
        Require(!disableAa.Checked, "AA debug override defaults off");
        disableAa.Checked = true;
        Require(form.CreateLaunchOptions().DisableAa, "AA override reaches launch options");
        var handsJump = Field<CheckBox>("handsJump");
        Require(handsJump.Checked && form.CreateLaunchOptions().HandsJump, "Hands Jump default on");
        handsJump.Checked = false;
        Require(!form.CreateLaunchOptions().HandsJump, "Hands Jump can still be disabled");
        handsJump.Checked = true;
        Require(form.CreateLaunchOptions().HandsJump, "Hands Jump launch option");
        typeof(MainForm).GetMethod("SaveSettings", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(form, null);
        Require(LauncherSettingsStore.Load(Path.Combine(testRoot, "ui-settings.json")).HandsJump, "Hands Jump persisted");
        Require(LauncherSettingsStore.Load(Path.Combine(testRoot, "ui-settings.json")).DisableAa, "AA override persists");
        Require(LauncherSettingsStore.Load(Path.Combine(testRoot, "ui-settings.json")).CaptureEyes,"eye capture persists");
        var presets = Field<ComboBox>("preset");
        var calibration = Field<CheckBox>("calibrateHands");
        Require(hands.Text == "Enable Hands" && hands.Checked, "main-page hands default");
        foreach (var index in new[] { 1, 2, 0 })
        {
            hands.Checked = false;
            handsJump.Checked = false;
            presets.SelectedIndex = index;
            Require(hands.Checked, "all named profiles enable hands");
            Require(handsJump.Checked, "all named profiles enable Hands Jump");
        }
        presets.SelectedIndex = 3;
        var renderer=Field<ComboBox>("rendererMode");
        Require(form.CreateLaunchOptions().RendererMode=="AER","fresh installation defaults to AER");
        Require(renderer.Items.Count == 3 && renderer.Items[0].ToString() == "AER"
            && renderer.Items[1].ToString() == "Native Stereo Experimental", "exact renderer choices");
        renderer.SelectedIndex=1;
        Require(form.CreateLaunchOptions().RendererMode == "NATIVE", "Native selection");
        Require(Field<CheckBox>("useFsrUpscaling").Enabled, "Native FSR control enabled");
        renderer.SelectedIndex=2;
        Require(form.CreateLaunchOptions().RendererMode == VulkanSfs.Key, "SFS selection");
        Require(renderer.Items[2].ToString() == VulkanSfs.Label, "technical SFS label");
        Require(RendererSelection.Index(VulkanSfs.Key)==2, "SFS settings restore");
        renderer.SelectedIndex=1;
        var migrationDirectory=Path.Combine(testRoot,"renderer-migration");
        Directory.CreateDirectory(migrationDirectory);
        foreach(var old in new[]{"NATIVE_MULTIVIEW","NATIVE_MULTIVIEW_VISIBLE","NATIVE_MULTIVIEW_HYBRID","NATIVE_CPU_RECORDING","NATIVE"}) {
            LauncherSettingsStore.Save(Path.Combine(migrationDirectory,"settings.json"),new LauncherSettings {RendererMode=old});
            Require(LauncherSettingsStore.Load(Path.Combine(migrationDirectory,"settings.json")).RendererMode=="NATIVE","legacy Native migration");
        }
        foreach(var old in new[]{"AFW","", "unknown"}) Require(RendererSelection.Normalize(old)=="AER","obsolete/default renderer migrates to AER");
        foreach(var marker in RendererSelection.ObsoleteMarkers) File.WriteAllText(Path.Combine(migrationDirectory,marker),"");
        RendererSelection.ClearObsoleteMarkers(migrationDirectory);
        Require(!RendererSelection.ObsoleteMarkers.Any(marker=>File.Exists(Path.Combine(migrationDirectory,marker))),"obsolete markers removed");
        var argumentBuilder=typeof(KharvoxRunner).GetMethod("BuildGameArguments",BindingFlags.Static|BindingFlags.NonPublic)!;
        foreach(var native in new[]{false,true}) {
            var args=((IEnumerable<string>)argumentBuilder.Invoke(null,new object[]{form.CreateLaunchOptions(),false,100m,native})!).ToArray();
            foreach(var name in new[]{"r_TAAResolveFilter","r_taaNegativeLodBiasVT","vt_lodBias","g_weaponkick"})
                Require(!args.Contains("+"+name), "removed debug CVar is never forced");
        }
        var previousRuntime = Environment.GetEnvironmentVariable("XR_RUNTIME_JSON");
        try
        {
            var manifest = Path.Combine(testRoot, "runtime.json");
            Environment.SetEnvironmentVariable("XR_RUNTIME_JSON", manifest);
            File.WriteAllText(manifest, "{\"runtime\":{\"library_path\":\"real_headset.dll\"}}");
            var nativeArgs=(IEnumerable<string>)argumentBuilder.Invoke(null,new object[]{form.CreateLaunchOptions(),false,100m,true})!;
            var diagnosticArgs=((IEnumerable<string>)argumentBuilder.Invoke(null,new object[]{form.CreateLaunchOptions(),false,150m,false})!).ToArray();
            Require(int.Parse(diagnosticArgs[Array.IndexOf(diagnosticArgs,"+r_windowWidth")+1])<=1280
                && int.Parse(diagnosticArgs[Array.IndexOf(diagnosticArgs,"+r_windowHeight")+1])<=720,
                "Default desktop window stays small even when eye render scale increases");
            Require(diagnosticArgs[Array.IndexOf(diagnosticArgs,"+r_fullscreen")+1]=="0",
                "window diagnostic uses an ordinary game-owned window");
            foreach(var runtimeLibrary in new[]{"virtualdesktop-openxr.dll", "oculus_openxr_64.dll", "vrclient_x64.dll"}) {
              var steam = runtimeLibrary == "vrclient_x64.dll";
              File.WriteAllText(manifest, "{\"runtime\":{\"library_path\":\""+runtimeLibrary+"\"}}");
              foreach(var disableAaRequested in new[]{false,true}) foreach(var native in new[]{false,true}) foreach(var scale in new[]{50m,75m,100m,110m,120m,150m,200m,300m,500m,1000m}) {
                disableAa.Checked=disableAaRequested;
                Field<NumericUpDown>("renderScale").Value=scale;
                var scaled=((IEnumerable<string>)argumentBuilder.Invoke(null,new object[]{form.CreateLaunchOptions(),false,KharvoxRunner.EffectiveRenderScale(scale,steam),native})!).ToArray();
                Require(scaled[Array.IndexOf(scaled,"+r_windowWidth")+1]==diagnosticArgs[Array.IndexOf(diagnosticArgs,"+r_windowWidth")+1],"desktop width is independent of runtime, renderer and RenderScale");
                Require(scaled[Array.IndexOf(scaled,"+r_windowHeight")+1]==diagnosticArgs[Array.IndexOf(diagnosticArgs,"+r_windowHeight")+1],"desktop height is independent of runtime, renderer and RenderScale");
                Require(scaled[Array.IndexOf(scaled,"+rs_enable")+1]=="0","no double scaling in engine");
                foreach(var setting in new[]{("r_antialiasing",disableAaRequested?"0":"2"),("r_motionblur","0"),("r_motionBlurQuality","0"),("r_filmGrainRatio","0"),("r_SSR","0"),("r_SSRQuality","0"),("r_sharpening","2")})
                    Require(scaled[Array.IndexOf(scaled,"+"+setting.Item1)+1]==setting.Item2,"renderer-specific AA and shared visual settings");
              }
            }
            foreach(var work in new[]{(640,480),(1280,720),(1920,1032),(2560,1440),(3840,2160),(7680,4320)}) {
                var mirror = KharvoxRunner.DesktopMirrorExtent(work.Item1,work.Item2);
                Require(mirror.Width<=1280 && mirror.Height<=720, "mirror stays compact on high-resolution desktops");
                Require(mirror.Width<=work.Item1-64 && mirror.Height<=work.Item2-64, "mirror fits smaller desktops including window frame");
                Require(Math.Abs(mirror.Width*9-mirror.Height*16)<=16, "mirror preserves source aspect to pixel rounding");
            }
            Require(KharvoxRunner.DesktopMirrorExtent(3840,2160)==(1280,720),"4K desktop uses compact 720p mirror");
            Field<NumericUpDown>("renderScale").Value=100m;
            File.WriteAllText(manifest,"{\"runtime\":{\"library_path\":\"real_headset.dll\"}}");
            var aerArgs=(IEnumerable<string>)argumentBuilder.Invoke(null,new object[]{form.CreateLaunchOptions(),false,100m,false})!;
            Require(string.Join(" ",nativeArgs).Contains("+r_useSMP 1"),"native serial renderer argument");
            Require(!aerArgs.Contains("+r_useSMP"),"headset AER restart retains original arguments");
            Require(!KharvoxRunner.NativeShadowDiagnosticArguments(false,true).Any(),
                "AER and AFW ignore the native shadow diagnostic, including recovery");
            Require(!KharvoxRunner.NativeShadowDiagnosticArguments(true,false).Any(),
                "ordinary native launch retains original shadow cache settings");
            Require(KharvoxRunner.NativeShadowDiagnosticArguments(true,true).SequenceEqual(new[] {
                "+r_shadowStaticMode","1","+r_shadowStaticMipTriThresholds","-1,-1,-1,-1,-1" }),
                "private native diagnostic disables only static image caching at startup");
            Require(!KharvoxRunner.NativeShadowFreshnessDiagnosticArguments(false,true).Any(),
                "AER and AFW ignore shadow freshness diagnostics, including recovery");
            Require(!KharvoxRunner.NativeShadowDiagnosticArguments(false,true,true).Any(),
                "AER recovery ignores both native cache markers, even a conflict");
            Require(KharvoxRunner.NativeShadowDiagnosticArguments(true,false,true).SequenceEqual(new[] {
                "+r_shadowStaticMode","1","+r_shadowStaticMipTriThresholds","-1,16,8,4,2" }),
                "cached comparison restores supported engine thresholds, not stale ages");
            bool cacheConflictRejected=false;
            try { KharvoxRunner.NativeShadowDiagnosticArguments(true,true,true).ToArray(); }
            catch(InvalidOperationException) { cacheConflictRejected=true; }
            Require(cacheConflictRejected,"conflicting native cache modes fail before game launch");
            Require(!KharvoxRunner.NativeShadowFreshnessDiagnosticArguments(true,false).Any(),
                "ordinary native launch retains original shadow update cadence");
            Require(KharvoxRunner.NativeShadowFreshnessDiagnosticArguments(true,true).SequenceEqual(new[] {
                "+r_shadowMaxStaleFrames","0,0,0,0,0","+r_shadowParallelMaxStaleFrames","0,0,0,0" }),
                "private native freshness diagnostic changes only permitted stale ages");
            const string startupMarker = "runtimeDir=D:\\release\\.";
            Require(KharvoxRunner.AnalyzeStartupLog(
                    "[KHARVOX] " + startupMarker + "\n[KHARVOX][XR] Frame 120 stable mode=PROJECTION result=0",
                    startupMarker) == KharvoxRunner.StartupState.Healthy,
                "minimal operational log confirms startup without extended diagnostics");
            Require(KharvoxRunner.PreserveGameAfterUnconfirmedStartup(KharvoxRunner.StartupState.TimedOut)
                    && KharvoxRunner.PreserveGameAfterUnconfirmedStartup(KharvoxRunner.StartupState.PresentStreamStalled),
                "missing or stalled diagnostics must not terminate a live game");
            Require(!KharvoxRunner.PreserveGameAfterUnconfirmedStartup(KharvoxRunner.StartupState.DeviceLost)
                    && !KharvoxRunner.PreserveGameAfterUnconfirmedStartup(KharvoxRunner.StartupState.Exited),
                "concrete startup failures still follow the error path");
            Require(KharvoxRunner.AnalyzeStartupLog(
                    startupMarker + "\n[KHARVOX][XR] State -> 3\n[XR-PACING] shouldRender=0",
                    startupMarker) == KharvoxRunner.StartupState.RuntimeNotRendering,
                "synchronized runtime withholding frames is distinct from renderer failure");
            Require(KharvoxRunner.AnalyzeStartupLog(
                    startupMarker + "\n[KHARVOX][XR] State -> 3\n[XR-PACING] shouldRender=0\nFrame 120 stable mode=QUAD",
                    startupMarker) == KharvoxRunner.StartupState.Healthy,
                "later stable frames supersede an initial runtime visibility delay");
            Require(KharvoxRunner.AnalyzeStartupLog(
                    startupMarker + "\n[KHARVOX][XR] State -> 3\n[XR-PACING] shouldRender=0\n[Present 1]\n[XR-PACING] shouldRender=1",
                    startupMarker) == KharvoxRunner.StartupState.PresentObserved,
                "latest runtime render permission clears an earlier visibility delay");
            Require(KharvoxRunner.AnalyzeStartupLog(
                    "runtimeDir=D:\\other\\.\n[KHARVOX][XR] State -> 3\n[XR-PACING] shouldRender=0",
                    startupMarker) == KharvoxRunner.StartupState.TimedOut,
                "runtime visibility diagnosis requires this launch's layer marker");
            File.WriteAllText(manifest, "{\"runtime\":{\"library_path\":\"D:/bin/openxr_simulator.dll\"}}");
            var simArgs=(IEnumerable<string>)argumentBuilder.Invoke(null,new object[]{form.CreateLaunchOptions(),false,100m,false})!;
            Require(string.Join(" ",simArgs).Contains("+r_useSMP 1"),"simulator AER recovery stays on inline thread");
            Require(!KharvoxRunner.IsSimulatorManifest("{invalid"), "malformed runtime manifest is not simulator");
            Require(!KharvoxRunner.IsSimulatorManifest("{\"runtime\":{\"library_path\":\"openxr_simulator.dll.other\"}}"),
                "simulator requires exact runtime library filename");
        }
        finally { Environment.SetEnvironmentVariable("XR_RUNTIME_JSON", previousRuntime); }
        renderer.SelectedIndex=0;
        Require(form.CreateLaunchOptions().RendererMode=="AER","AER remains selectable");
        renderer.SelectedIndex=1;
        hands.Checked = false;
        Require(!hands.Checked, "Custom permits hands off");
        calibration.Checked = true;
        Require(hands.Checked, "calibration enables hands");
        hands.Checked = false;
        Require(!calibration.Checked && !form.CreateLaunchOptions().ShowHands,
            "main checkbox off also disables calibration override");
        hands.Checked = true;
        form.CreateControl();
        form.PerformLayout();
        using var bitmap = new Bitmap(form.ClientSize.Width, form.ClientSize.Height);
        form.DrawToBitmap(bitmap, form.ClientRectangle);
        Require(hands.FindForm() == form && hands.Parent == laser.Parent
            && hands.Left >= laser.Right, "hands is on main page right of laser");
        Require(handsJump.Parent == hands.Parent && handsJump.Left == hands.Left
            && handsJump.Top >= hands.Bottom, "Hands Jump below Enable Hands");
        foreach (var check in new[] { hands, laser, handsJump, Field<CheckBox>("usePsvr2Toolkit") })
            Require(check.Width >= TextRenderer.MeasureText(check.Text, check.Font).Width + 18,
                "checkbox text fits: " + check.Text);
        var previewDirectory = Environment.GetEnvironmentVariable("KHARVOX_SELFTEST_LAYOUT_DIR");
        if (!string.IsNullOrWhiteSpace(previewDirectory))
        {
            Directory.CreateDirectory(previewDirectory);
            form.ShowInTaskbar = false;
            form.StartPosition = FormStartPosition.Manual;
            form.Location = new Point(-32000, -32000);
            form.Show();
            form.PerformLayout();
            Application.DoEvents();
            form.Refresh();
            Require(laser.PointToScreen(Point.Empty).X == Field<ComboBox>("backWeapon").PointToScreen(Point.Empty).X
                && laser.Left == Field<CheckBox>("usePsvr2Toolkit").Left, "laser and toolkit align with shoulder field");
            Application.DoEvents();
            using var visibleBitmap = new Bitmap(form.Width, form.Height);
            form.DrawToBitmap(visibleBitmap, new Rectangle(Point.Empty, form.Size));
            visibleBitmap.Save(Path.Combine(previewDirectory, "launcher-hands.png"));
            form.Hide();
        }
        using var dev = new DevModeForm(new ComboBox(), new ComboBox(), new ComboBox(),
            new CheckBox(), new CheckBox(), new CheckBox(), new Label(),
            disableAa, captureEyes);
        if (!string.IsNullOrWhiteSpace(previewDirectory)) {
            dev.StartPosition=FormStartPosition.Manual;dev.Location=new Point(-32000,-32000);
            dev.Show();Application.DoEvents();dev.Refresh();Application.DoEvents();
            using var preview=new Bitmap(dev.Width,dev.Height);
            dev.DrawToBitmap(preview,new Rectangle(Point.Empty,dev.Size));
            preview.Save(Path.Combine(previewDirectory,"debug-cvars.png"));dev.Hide();
        }
        Require(hands.FindForm() == form, "opening developer controls does not reparent hands");
        Field<System.Windows.Forms.Timer>("runtimeStatusTimer").Stop();
    }

    private static void RequireImmersivePreset(
        int presetIndex, bool regularCinematicsInCineWindow, string name)
    {
        Require(LauncherPresetPolicy.TryGet(presetIndex, out var preset),
            name + " preset exists");
        Require(preset.RendererMode == "AER" && preset.RenderScale == 100m
            && !preset.UseFsrUpscaling, name + " rendering defaults");
        Require(preset.ImmersiveMode && preset.CinematicFreelook
            && preset.RegularCinematicsInCineWindow
                == regularCinematicsInCineWindow
            && !preset.CineWindowFollowsHeadset,
            name + " cinematic defaults");
        Require(preset.PhysicalGloryKills
            && preset.PhysicalGloryKillSpeed == 2.8m
            && preset.GloryKillHands == 2,
            name + " Glory Kill defaults");
        Require(preset.ShoulderWeapon == 3 && preset.VirtualGunstock
            && !preset.LaserSight && preset.EnableHands && !preset.EnableBhaptics
            && !preset.UsePsvr2Toolkit,
            name + " weapon and optional-feature defaults");
        Require(preset.TurnMode == 0 && preset.MovementDirection == 0
            && preset.TurnSpeed == 230
            && preset.SnapAngle == 45m && !preset.LeftHandMode
            && preset.LeftHandSwapMode == 0,
            name + " movement defaults");
    }

    private static void VerifySfsPackage(string root)
    {
        var runtime = Path.Combine(root, "sfs-fixture");
        Directory.CreateDirectory(runtime);
        var layer = Path.Combine(runtime, "KharvoxLayer.dll");
        File.WriteAllText(layer, "Hash fixture only; never loaded or packaged");
        using var sha = System.Security.Cryptography.SHA256.Create();
        File.WriteAllLines(Path.Combine(runtime, "native_sfs_build.txt"), new[] {
            "KHARVOX_NATIVE_SFS_1", BitConverter.ToString(sha.ComputeHash(File.ReadAllBytes(layer))).Replace("-", "") });
        var licenses = Path.Combine(runtime, "sfs-compiler-licenses");
        Directory.CreateDirectory(licenses);
        foreach (var name in new[] { "SPIRV-Cross.txt", "glslang.txt", "SPIRV-Tools.txt" })
            File.WriteAllText(Path.Combine(licenses, name), "Test fixture");
        bool rejected = false;
        try { VulkanSfs.EnsureAvailable(runtime); } catch (InvalidOperationException) { rejected = true; }
        Require(rejected, "SFS package without local profile rejected");
        var profile = Path.Combine(runtime, "sfs-profile");
        Directory.CreateDirectory(profile);
        var shader = Path.Combine(profile, "fixture.spv");
        File.WriteAllBytes(shader, new byte[20]);
        for (var i = 1; i < 75; ++i)
            using (var writer = new BinaryWriter(File.Create(Path.Combine(profile, "fixture" + i + ".spv"))))
                foreach (var word in new uint[] { 0x07230203, 0x00010000, 0, 1, 0 }) writer.Write(word);
        rejected = false;
        try { VulkanSfs.EnsureAvailable(runtime); } catch (InvalidOperationException) { rejected = true; }
        Require(rejected, "SFS corrupt SPIR-V header rejected");
        using (var writer = new BinaryWriter(File.Create(shader)))
            foreach (var word in new uint[] { 0x07230203, 0x00010000, 0, 1, 0 }) writer.Write(word);
        var start = new System.Diagnostics.ProcessStartInfo { UseShellExecute = false };
        VulkanSfs.Configure(start, runtime);
        Require(start.EnvironmentVariables["KHARVOX_SFS_NATIVE_VR"] == "1" &&
            start.EnvironmentVariables["KHARVOX_SFS_PROFILE"] == profile, "SFS child selects native producer and local profile");
        VulkanSfs.ClearEnvironment(start);
        Require(start.EnvironmentVariables["KHARVOX_SFS_NATIVE_VR"] == null &&
            start.EnvironmentVariables["KHARVOX_SFS_NATIVE_PROBE"] == null &&
            start.EnvironmentVariables["KHARVOX_SFS_PROFILE"] == null, "AER child cannot inherit SFS flags");
        File.AppendAllText(layer, "changed");
        rejected = false;
        try { VulkanSfs.EnsureAvailable(runtime); } catch (InvalidOperationException) { rejected = true; }
        Require(rejected, "SFS manifest for a different DLL rejected");
    }

    private static void Require(bool condition, string check)
    {
        if (!condition) throw new InvalidOperationException("Self-test failed: " + check);
    }
}
