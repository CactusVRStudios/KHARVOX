namespace KharvoxLauncher;

// Same control-to-option mapping as MainForm.CreateLaunchOptions, without
// constructing a form or embedding its demo/audio/art resources in the host.
internal static class SettingsLaunchOptions
{
    private static readonly string[] CalibrationWeapons = {
        "pistol", "shotgun", "heavy_assault_rifle", "plasma_rifle", "rocket_launcher",
        "super_shotgun", "gauss_cannon", "chaingun", "bfg", "chainsaw", "fists",
        "assault_rifle", "arc_cannon", "mancubus_gland" };
    private static readonly string[] BackWeapons = {
        "pistol", "shotgun", "plasma_rifle", "heavy_assault_rifle", "rocket_launcher",
        "super_shotgun", "gauss_cannon", "chaingun", "bfg", "chainsaw" };

    internal static KharvoxLaunchOptions From(LauncherSettings settings) => new(
        immersiveMode: settings.Intense,
        cinematicFreelook: settings.Intense && settings.CinematicFreelook,
        otherCinematicsInQuad: settings.Intense && settings.OtherCinematicsInQuad,
        cinewindowFollowsHeadset: settings.CinewindowFollowsHeadset,
        rendererMode: settings.RendererMode,
        renderScale: Math.Max(50, settings.RenderScale),
        useFsrUpscaling: settings.UseFsrUpscaling,
        gameDirectory: settings.GamePath,
        turnMode: settings.TurnMode switch { 1 => "Snap", 2 => "Off", _ => "Smooth" },
        movementDirection: settings.MovementDirection == 1 ? "off-hand" : "head",
        smoothTurnSpeed: Math.Max(150, Math.Min(400, settings.SmoothSpeed)),
        snapTurnAngle: Math.Max(10, Math.Min(180, settings.SnapAngle)),
        turnDeadzone: .35m,
        twoHandCalibration: settings.WeaponMode == 1,
        twoHandCalibrationWeapon: CalibrationWeapons[Math.Max(0, Math.Min(CalibrationWeapons.Length - 1, settings.CalibrationWeapon))],
        twoHandAlignment: settings.GripAlignment == 1 ? "side-grip" : "barrel",
        virtualGunstock: settings.VirtualGunstock,
        physicalGlorykill: settings.PhysicalGlorykill,
        physicalGlorykillSpeed: Math.Max(1, Math.Min(4, settings.PhysicalGlorykillSpeed)),
        physicalGlorykillHands: settings.PhysicalGlorykillHands switch { 0 => "left", 1 => "right", _ => "both" },
        leftHanded: settings.LeftHanded,
        leftHandSwapMode: settings.LeftHandSwapMode == 1 ? "buttons-and-sticks" : "buttons",
        laserSight: settings.LaserSight,
        hudDebugging: settings.HudDebugging,
        extendedLogging: settings.ExtendedLogging,
        showHands: settings.ShowHands,
        handCalibrationMode: settings.HandCalibrationMode != 0 ? "rotation" : "off",
        enableBhaptics: settings.EnableBhaptics,
        usePsvr2Toolkit: settings.UsePsvr2Toolkit,
        backWeapon: BackWeapons[Math.Max(0, Math.Min(BackWeapons.Length - 1, settings.BackWeapon))],
        handsJump: settings.HandsJump,
        disableAa: settings.DisableAa,
        captureEyes: settings.CaptureEyes,
        disableVrIntro: settings.DisableVrIntro,
        swapJumpCrouch: settings.SwapJumpCrouch,
        physicalCrouch: settings.PhysicalCrouch,
        physicalGrenadeThrow: settings.PhysicalGrenadeThrow);
}
