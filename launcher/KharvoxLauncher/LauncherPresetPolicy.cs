namespace KharvoxLauncher;

internal readonly struct LauncherPresetDefinition
{
    internal LauncherPresetDefinition(bool regularCinematicsInCineWindow)
    {
        RegularCinematicsInCineWindow = regularCinematicsInCineWindow;
    }

    internal string RendererMode => "AER";
    internal decimal RenderScale => 100m;
    internal bool UseFsrUpscaling => false;
    internal bool ImmersiveMode => true;
    internal bool CinematicFreelook => true;
    internal bool RegularCinematicsInCineWindow { get; }
    internal bool CineWindowFollowsHeadset => false;
    internal bool PhysicalGloryKills => true;
    internal decimal PhysicalGloryKillSpeed => 2.8m;
    internal int GloryKillHands => 2;
    internal int ShoulderWeapon => 3;
    internal bool VirtualGunstock => true;
    internal bool LaserSight => false;
    internal bool EnableHands => LauncherPresetPolicy.DefaultEnableHands;
    internal bool EnableBhaptics => false;
    internal bool UsePsvr2Toolkit => false;
    internal int TurnMode => 0;
    internal int MovementDirection => 0;
    internal int TurnSpeed => 230;
    internal decimal SnapAngle => 45m;
    internal bool LeftHandMode => false;
    internal int LeftHandSwapMode => 0;
}

internal static class LauncherPresetPolicy
{
    internal const bool DefaultEnableHands = true;
    internal static bool TryGet(int presetIndex, out LauncherPresetDefinition definition)
    {
        // Recommended and Intense intentionally share the same high-immersion
        // baseline. Recommended alone routes regular scripted cinematics to
        // the Cine Window; Intense keeps them immersive.
        if (presetIndex == 0 || presetIndex == 2)
        {
            definition = new LauncherPresetDefinition(
                regularCinematicsInCineWindow: presetIndex == 0);
            return true;
        }
        definition = default;
        return false;
    }
}
