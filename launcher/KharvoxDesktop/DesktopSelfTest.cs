using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Threading;
using KharvoxLauncher;
using System.Diagnostics;
using System.Text.Json;

namespace KharvoxDesktop;

internal static class DesktopSelfTest
{
    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    internal static int Run(string[] args)
    {
        var lines = new List<string>();
        var directory = Path.Combine(Path.GetTempPath(), "KHARVOX-Desktop-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var logIndex = Array.IndexOf(args, "--log");
        var log = logIndex >= 0 && args.Length > logIndex + 1 ? args[logIndex + 1] : Path.Combine(directory, "result.log");
        int result;
        try
        {
            LauncherLanguage.VerifyCatalog();
            var languageRuntime = Path.Combine(directory, "language");
            Directory.CreateDirectory(languageRuntime);
            var languageProfile = Path.Combine(languageRuntime, "settings.json");
            var languageVm = new LauncherViewModel(languageRuntime, languageProfile);
            Check(languageVm.LanguageIndex == 0 && languageVm.Presets[0].Label == "Recommandé",
                "A missing language preference must default to French.");
            languageVm.RendererIndex = 1; languageVm.LeftHanded = true;
            languageVm.BackWeapon = 7; languageVm.PresetIndex = 3;
            languageVm.Save();
            var savedOptions = File.ReadAllBytes(languageProfile);
            var choices = languageVm.BackWeapons;
            languageVm.LanguageIndex = 1;
            Check(languageVm.Presets[0].Label == "Recommended" && languageVm.BackWeapons[7].Label == "Chaingun"
                && ReferenceEquals(choices, languageVm.BackWeapons)
                && savedOptions.SequenceEqual(File.ReadAllBytes(languageProfile)),
                "Language changes must update labels without touching VR preferences or item identities.");
            var restoredLanguage = new LauncherViewModel(languageRuntime, languageProfile);
            Check(restoredLanguage.LanguageIndex == 1 && restoredLanguage.RendererIndex == 1
                && restoredLanguage.LeftHanded && restoredLanguage.BackWeapon == 7,
                "English must persist independently of renderer, handedness and weapon choices.");
            restoredLanguage.LanguageIndex = 0;
            Check(LauncherLanguage.Read(Path.Combine(languageRuntime, "launcher_language.cfg")) == 0,
                "Switching back to French must persist.");
            var languagePreviewDirectory = Path.Combine(directory, "language-preview");
            Directory.CreateDirectory(languagePreviewDirectory);
            var languagePreview = new LauncherViewModel(languagePreviewDirectory, Path.Combine(languagePreviewDirectory,"settings.json"), true);
            languagePreview.LanguageIndex = 1;
            Check(!File.Exists(Path.Combine(languagePreviewDirectory,"launcher_language.cfg")),
                "Preview language changes must not write a preference.");
            languagePreview.LanguageIndex = 0;
            lines.Add("PASS: French default, live English labels, language persistence and VR preference isolation.");
            var comparisonExpected = args.Contains("--performance-comparison") || string.Equals(
                Path.GetFileNameWithoutExtension(Environment.ProcessPath), "KharvoxPerformance", StringComparison.OrdinalIgnoreCase);
            if (comparisonExpected)
                Check(Environment.GetEnvironmentVariable("KHARVOX_PERFORMANCE_COMPARISON") == "1",
                    "The comparison executable/argument must arm the inherited runtime mode.");
            lines.Add("PASS: performance comparison mode=" + (comparisonExpected ? "enabled" : "normal") + ".");
            var profile = Path.Combine(directory, "profile.json");
            var vm = new LauncherViewModel(AppContext.BaseDirectory, profile);
            Check(vm.Hands && vm.Crouch && vm.Throw, "First portable profile enables the requested corrections.");
            vm.GamePath = Path.Combine(directory, "DOOM-fixture");
            vm.RendererIndex = 0; vm.RenderScale = 90; vm.LeftHanded = true;
            vm.HandSwapMode = 1; vm.SwapJump = true; vm.CineWindow = false;
            vm.Immersive = true; vm.Freelook = true; vm.Bhaptics = false; vm.Psvr2 = false;
            vm.Save();
            var reloaded = new LauncherViewModel(AppContext.BaseDirectory, profile);
            Check(reloaded.RendererIndex == 0 && reloaded.RenderScale == 90 && reloaded.LeftHanded
                && reloaded.HandSwapMode == 1 && reloaded.SwapJump && !reloaded.CineWindow
                && reloaded.Crouch && reloaded.Throw, "Portable settings must survive a restart.");
            lines.Add("PASS: SFS, left-handed controls and corrections persist together.");

            reloaded.RendererIndex = 1; reloaded.Crouch = false; reloaded.Throw = false;
            reloaded.Hands = false; reloaded.Save();
            var aer = new LauncherViewModel(AppContext.BaseDirectory, profile);
            Check(aer.RendererIndex == 1 && !aer.Crouch && !aer.Throw && !aer.Hands,
                "AER and explicit disabled corrections must survive a restart.");
            lines.Add("PASS: AER and disabled corrections remain selected.");

            LauncherSettingsStore.Save(profile, new LauncherSettings {
                SettingsVersion = 34, Preset = 3, PhysicalCrouch = true, PhysicalGrenadeThrow = true,
                RenderScale = 100, SmoothSpeed = 230, SnapAngle = 45 });
            var migrated = new LauncherViewModel(AppContext.BaseDirectory, profile);
            Check(!migrated.Crouch && !migrated.Throw, "Old profiles must not silently opt into physical controls.");
            migrated.PresetIndex = 2;
            Check(migrated.Immersive && !migrated.CineWindow && !migrated.Crouch && !migrated.Throw,
                "Immersive preset must retain the existing opt-in defaults.");
            migrated.EnableCorrections();
            Check(migrated.PresetIndex == 3 && migrated.Hands && migrated.Crouch && migrated.Throw,
                "Enable corrections must select a custom profile.");
            lines.Add("PASS: migration, presets and one-click correction selection.");

            migrated.CalibrateHud = true; migrated.CalibrateHands = true;
            Check(migrated.CalibrateHands && !migrated.CalibrateHud, "Hand calibration owns the numeric keypad.");
            migrated.CalibrateHud = true;
            Check(migrated.CalibrateHud && !migrated.CalibrateHands, "HUD and hand calibration must remain mutually exclusive.");
            lines.Add("PASS: calibration controls preserve keypad ownership.");

            var previewPath = Path.Combine(directory, "preview.json");
            var preview = new LauncherViewModel(directory, previewPath, true);
            preview.Save(); preview.LaunchOrStop();
            Check(!File.Exists(previewPath) && !preview.Running && !preview.HasError,
                "Preview must not save preferences or launch the game.");
            var missing = new LauncherViewModel(directory, Path.Combine(directory, "missing.json"));
            missing.GamePath = directory; missing.LaunchOrStop();
            Check(missing.HasError && missing.CanEdit && missing.CanLaunch && !missing.Running,
                "Invalid installation must return a usable error state without starting a session.");
            lines.Add("PASS: preview isolation and launch error recovery.");

            var backendIndex = Array.IndexOf(args, "--backend");
            var backend = backendIndex >= 0 && args.Length > backendIndex + 1
                ? args[backendIndex + 1] : Path.Combine(AppContext.BaseDirectory, "KharvoxLaunchHost.exe");
            Check(File.Exists(backend), "The launch host is required for the interoperation tests.");
            if (File.Exists(Path.Combine(AppContext.BaseDirectory, "KharvoxLayer.dll")))
            {
                VulkanSfs.EnsureAvailable(AppContext.BaseDirectory);
                lines.Add("PASS: delivered native SFS DLL, matching hash, compiler notices and 75 shaders.");
            }
            foreach (var renderer in new[] { 0, 1 })
            foreach (var left in new[] { false, true })
            {
                migrated.RendererIndex = renderer; migrated.LeftHanded = left;
                migrated.HandSwapMode = 1; migrated.SwapJump = true;
                migrated.Immersive = true; migrated.CineWindow = false;
                migrated.CalibrateHands = false; migrated.CalibrateHud = false;
                migrated.EnableCorrections(); migrated.Save();
                var options = ReadBackendOptions(backend, profile, directory);
                Check(options.GetProperty("RendererMode").GetString() == (renderer == 0 ? VulkanSfs.Key : "AER"),
                    "Renderer selection must reach the original backend.");
                foreach (var name in new[] { "PhysicalCrouch", "PhysicalGrenadeThrow", "ShowHands", "SwapJumpCrouch" })
                    Check(options.GetProperty(name).GetBoolean(), name + " must reach the original backend.");
                Check(options.GetProperty("LeftHanded").GetBoolean() == left, "Handedness must reach the original backend.");
                Check(!options.GetProperty("OtherCinematicsInQuad").GetBoolean(), "Immersive cinematics selection must reach the original backend.");
            }
            migrated.Crouch = migrated.Throw = false; migrated.Save();
            var disabled = ReadBackendOptions(backend, profile, directory);
            Check(!disabled.GetProperty("PhysicalCrouch").GetBoolean() && !disabled.GetProperty("PhysicalGrenadeThrow").GetBoolean(),
                "Disabled physical controls must reach the original backend.");
            lines.Add("PASS: launch host receives SFS/AER, both handedness modes, crouch, grenades and cinema choices.");
            migrated.TurnMode = 1; migrated.MoveMode = 1; migrated.CalibrationWeapon = 2;
            migrated.BackWeapon = 2; migrated.GripMode = 1; migrated.WeaponMode = 1;
            migrated.CalibrateHands = true; migrated.Save();
            var controls = ReadBackendOptions(backend, profile, directory);
            Check(controls.GetProperty("TurnMode").GetString() == "Snap"
                && controls.GetProperty("MovementDirection").GetString() == "off-hand"
                && controls.GetProperty("TwoHandCalibration").GetBoolean()
                && controls.GetProperty("TwoHandCalibrationWeapon").GetString() == "heavy_assault_rifle"
                && controls.GetProperty("BackWeapon").GetString() == "plasma_rifle"
                && controls.GetProperty("TwoHandAlignment").GetString() == "side-grip"
                && controls.GetProperty("HandCalibrationMode").GetString() == "rotation",
                "Launch host must retain the existing control/calibration key mappings.");
            lines.Add("PASS: launch host preserves turn, movement, shoulder weapon and calibration mappings.");
            result = 0;
        }
        catch (Exception error) { lines.Add("FAIL: " + error); result = 1; }
        File.WriteAllLines(log, lines);
        foreach (var line in lines) Console.WriteLine(line);
        return result;
    }

    private static JsonElement ReadBackendOptions(string backend, string profile, string directory)
    {
        var output = Path.Combine(directory, Guid.NewGuid().ToString("N") + ".json");
        var start = new ProcessStartInfo(backend) { UseShellExecute = false, CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = Path.GetDirectoryName(backend)! };
        foreach (var arg in new[] { "--desktop-options", profile, output }) start.ArgumentList.Add(arg);
        using var process = Process.Start(start)!;
        if (!process.WaitForExit(15000)) { process.Kill(); throw new TimeoutException("The launch backend did not return its options."); }
        Check(process.ExitCode == 0, "Backend options failed: " + (File.Exists(output) ? File.ReadAllText(output) : "no report"));
        using var document = JsonDocument.Parse(File.ReadAllText(output));
        return document.RootElement.Clone();
    }

    internal static int Capture(string output, string language = "fr")
    {
        try
        {
            var window = new MainWindow();
            window.Show(); Dispatcher.UIThread.RunJobs();
              Check(window.Title == "DOOM VR PLUS · v1.4.1" && window.Icon is null,
                  "The launcher must use its new title without the KHARVOX icon.");
              Check(window.FindControl<AnimatedLauncherFrame>("LauncherFrame")?.FrameCount == 6,
                  "All six frames of the supplied transparent GIF must decode.");
            var vm = (LauncherViewModel)window.DataContext!;
            vm.GamePath = ""; // Public screenshots must not expose an installation path.
            var versionBadge = window.FindControl<TextBlock>("VersionBadge")!;
            Check(versionBadge.Text == vm.VersionLabel && versionBadge.Text == "v1.4.1"
                && versionBadge.IsEffectivelyVisible && versionBadge.Bounds.Width > 0,
                "The compiled launcher version must be visible in the header.");
            var languageBox = window.FindControl<ComboBox>("LanguageBox")!;
            var menuPoint = languageBox.TranslatePoint(new Avalonia.Point(languageBox.Bounds.Width / 2,
                languageBox.Bounds.Height / 2), window)!.Value;
            window.MouseDown(menuPoint, Avalonia.Input.MouseButton.Left);
            window.MouseUp(menuPoint, Avalonia.Input.MouseButton.Left);
            Dispatcher.UIThread.RunJobs();
            Check(languageBox.IsDropDownOpen, "Clicking the language menu must open it instead of dragging the window.");
            languageBox.IsDropDownOpen = false;
            Check(languageBox.SelectedIndex == 0
                && window.FindControl<TextBlock>("PlayNavigationLabel")!.Text == "Jouer",
                "The visible language menu and navigation must default to French.");
            var beforeRenderer = vm.RendererIndex;
            var beforePreset = vm.PresetIndex;
            var beforeWeapon = vm.BackWeapon;
            var beforeHandedness = vm.LeftHanded;
            languageBox.SelectedIndex = 1; Dispatcher.UIThread.RunJobs();
            Check(vm.LanguageIndex == 1 && vm.PrimaryButtonText == "LAUNCH IN VR"
                && window.FindControl<TextBlock>("PlayNavigationLabel")!.Text == "Play"
                && window.FindControl<TextBlock>("FeatureSummaryText")!.Text!.StartsWith("Praetor hands")
                && vm.RendererIndex == beforeRenderer && vm.PresetIndex == beforePreset
                && vm.BackWeapon == beforeWeapon && vm.LeftHanded == beforeHandedness,
                "The actual language menu must translate the interface without changing VR selections.");
            languageBox.SelectedIndex = language == "en" ? 1 : 0;
            Dispatcher.UIThread.RunJobs();
            Check(window.FindControl<ToggleSwitch>("CrouchToggle")!.IsChecked == vm.Crouch,
                "Physical crouch control must display its saved value.");
            var toggle = window.FindControl<ToggleSwitch>("ThrowToggle")!;
            toggle.IsChecked = false; Dispatcher.UIThread.RunJobs();
            Check(!vm.Throw, "Grenade control must update the settings binding.");
            toggle.IsChecked = true; Dispatcher.UIThread.RunJobs();
            Check(vm.Throw, "Grenade control must enable the actual setting.");
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(output))!);
            for (var page = 0; page < 4; ++page)
            {
                vm.ActivePage = page; Dispatcher.UIThread.RunJobs();
                using var frame = window.CaptureRenderedFrame() ?? throw new InvalidOperationException("No rendered frame.");
                var path = page == 0 ? output : Path.Combine(Path.GetDirectoryName(output)!,
                    Path.GetFileNameWithoutExtension(output) + "-" + page + Path.GetExtension(output));
                frame.Save(path);
            }
            window.Close();
            return 0;
        }
        catch (Exception error)
        {
            File.WriteAllText(output + ".error.txt", error.ToString());
            return 1;
        }
    }
}
