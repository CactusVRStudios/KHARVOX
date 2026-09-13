using System.Web.Script.Serialization;

namespace KharvoxLauncher;

internal sealed class LauncherSettings
{
    internal const int CurrentVersion = 32;

    public int SettingsVersion { get; set; }
    public int Preset { get; set; }
    public bool Intense { get; set; }
    public bool CinematicFreelook { get; set; } = true;
    public bool OtherCinematicsInQuad { get; set; }
    public bool CinewindowFollowsHeadset { get; set; }
    public string RendererMode { get; set; } = string.Empty;
    public decimal RenderScale { get; set; }
    public bool UseFsrUpscaling { get; set; }
    public string GamePath { get; set; } = string.Empty;
    public int TurnMode { get; set; }
    public int MovementDirection { get; set; }
    public decimal SmoothSpeed { get; set; }
    public decimal SnapAngle { get; set; }
    public decimal Deadzone { get; set; }
    public int WeaponMode { get; set; }
    public int CalibrationWeapon { get; set; } = 1;
    public int BackWeapon { get; set; } = 1;
    public int GripAlignment { get; set; }
    public bool VirtualGunstock { get; set; }
    public bool PhysicalGlorykill { get; set; }
    public decimal PhysicalGlorykillSpeed { get; set; } = 2.80m;
    public int PhysicalGlorykillHands { get; set; } = 2;
    public bool LeftHanded { get; set; }
    public int LeftHandSwapMode { get; set; }
    public bool LaserSight { get; set; }
    public bool HudDebugging { get; set; }
    public bool ExtendedLogging { get; set; }
    public bool CaptureEyes { get; set; }
    public bool DisableAa { get; set; }
    public bool HandsJump { get; set; } = true;
    public bool DisableVrIntro { get; set; } = false;
    public bool ShowHands { get; set; } = LauncherPresetPolicy.DefaultEnableHands;
    public int HandCalibrationMode { get; set; }
    public bool EnableBhaptics { get; set; }
    public bool UsePsvr2Toolkit { get; set; }

    public LauncherSettings() { }

    public LauncherSettings(int preset, string gamePath, bool intense, bool cinematicFreelook,
        bool otherCinematicsInQuad, bool cinewindowFollowsHeadset,
        string rendererMode, decimal renderScale, bool useFsrUpscaling,
        int turnMode, int movementDirection, decimal smoothSpeed,
        decimal snapAngle, decimal deadzone, int weaponMode, int calibrationWeapon,
        int backWeapon,
        int gripAlignment, bool virtualGunstock, bool physicalGlorykill,
        decimal physicalGlorykillSpeed, int physicalGlorykillHands, bool leftHanded,
        int leftHandSwapMode, bool laserSight, bool hudDebugging, bool extendedLogging,
        bool showHands, int handCalibrationMode,
        bool enableBhaptics, bool usePsvr2Toolkit, bool handsJump = true, bool disableAa = false, bool captureEyes = false)
    {
        SettingsVersion = CurrentVersion;
        Preset = preset;
        GamePath = gamePath;
        Intense = intense;
        CinematicFreelook = cinematicFreelook;
        OtherCinematicsInQuad = otherCinematicsInQuad;
        CinewindowFollowsHeadset = cinewindowFollowsHeadset;
        RendererMode = rendererMode;
        RenderScale = renderScale;
        UseFsrUpscaling = useFsrUpscaling;
        TurnMode = turnMode;
        MovementDirection = movementDirection;
        SmoothSpeed = smoothSpeed;
        SnapAngle = snapAngle;
        Deadzone = deadzone;
        WeaponMode = weaponMode;
        CalibrationWeapon = calibrationWeapon;
        BackWeapon = backWeapon;
        GripAlignment = gripAlignment;
        VirtualGunstock = virtualGunstock;
        PhysicalGlorykill = physicalGlorykill;
        PhysicalGlorykillSpeed = physicalGlorykillSpeed;
        PhysicalGlorykillHands = physicalGlorykillHands;
        LeftHanded = leftHanded;
        LeftHandSwapMode = leftHandSwapMode;
        LaserSight = laserSight;
        HudDebugging = hudDebugging;
        ExtendedLogging = extendedLogging;
        ShowHands = showHands;
        HandsJump = handsJump;
        DisableAa = disableAa;
        CaptureEyes = captureEyes;

        HandCalibrationMode = handCalibrationMode is 1 or 2 ? 1 : 0;
        EnableBhaptics = enableBhaptics;
        UsePsvr2Toolkit = usePsvr2Toolkit;
    }
}

internal static class LauncherSettingsStore
{
    internal static string DefaultPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "KHARVOX", "settings.json");

    internal static LauncherSettings Load(string path)
    {
        var settings = new JavaScriptSerializer().Deserialize<LauncherSettings>(File.ReadAllText(path));
        if (settings is null)
            throw new InvalidDataException("The KHARVOX settings file is empty.");
        // bHaptics is opt-in. A value injected into an older settings schema
        // must not silently activate a local bridge after an upgrade.
        if (settings.SettingsVersion < 17)
            settings.EnableBhaptics = false;
        // Older schemas predate the body-relative back-weapon gesture. Combat
        // Shotgun is both its documented default and safe native fallback.
        if (settings.SettingsVersion < 19)
            settings.BackWeapon = 1;
        // Adaptive triggers are opt-in. Older schemas must never activate a
        // newly installed local Toolkit bridge merely because an unknown JSON
        // field was injected before the upgrade.
        if (settings.SettingsVersion < 21)
            settings.UsePsvr2Toolkit = false;
        // FSR changes both the source/target resolution relationship and adds
        // compute work. It is always an explicit opt-in after migration.
        if (settings.SettingsVersion < 22)
            settings.UseFsrUpscaling = false;
        // Head-relative locomotion is the established behaviour. Older files
        // must not opt into controller-relative movement implicitly.
        if (settings.SettingsVersion < 24)
            settings.MovementDirection = 0;
        // Extended logging redirects and records large amounts of diagnostic
        // output. Existing settings must never enable it implicitly.
        if (settings.SettingsVersion < 25)
            settings.ExtendedLogging = false;
        // Hands are now a default-on main-page feature. Migrate the former
        // developer opt-in once; preserve explicit on/off choices in v29+.
        if (settings.SettingsVersion < 29)
            settings.ShowHands = LauncherPresetPolicy.DefaultEnableHands;
        // Hand calibration owns the numeric keypad and is intentionally an
        // explicit developer selection in the new schema.
        if (settings.SettingsVersion < 27
            || settings.HandCalibrationMode is < 0 or > 2)
            settings.HandCalibrationMode = 0;
        // One opt-in checkbox now owns both adjustments. Older rotation and
        // position selections remain enabled; Num + selects the live mode.
        if (settings.HandCalibrationMode == 2)
            settings.HandCalibrationMode = 1;
        // The old TSSAA-only diagnostic must not silently disable the new SMAA default.
        if (settings.SettingsVersion < 30)
            settings.DisableAa = false;
        if (settings.SettingsVersion < 31) settings.DisableVrIntro = false;
        settings.RendererMode = RendererSelection.Normalize(settings.RendererMode);
        return settings;
    }

    internal static void Save(string path, LauncherSettings settings)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        File.WriteAllText(path, new JavaScriptSerializer().Serialize(settings));
    }
}
