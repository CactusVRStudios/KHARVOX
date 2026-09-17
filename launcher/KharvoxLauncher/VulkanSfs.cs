using System.Diagnostics;
using System.Text.RegularExpressions;

namespace KharvoxLauncher;

// External, user-supplied provider. Keep its original identity and notices;
// the KHARVOX renderer name describes the transport, not authorship.
internal static class VulkanSfs
{
    internal const string Key = "VULKAN_SFS";
    internal const string Label = "Vulkan Single-Frame Stereo (Test)";
    internal const string Blocker = "Vulkan Single-Frame Stereo 0.96 is not ready for gameplay. The local provider exits with 0xC0000374 even without KHARVOX. See Docs/VULKAN_SFS_096.md. Use AER until the provider startup and eye transport are verified.";

    internal static void EnsureAvailable()
    {
        // Developer probes use their own process environment. Do not expose a
        // known failing provider or an unverified SBS layout as a playable mode.
        throw new InvalidOperationException(Blocker);
    }

    internal static void Configure(ProcessStartInfo start, string runtime)
    {
        var root = Environment.GetEnvironmentVariable("KHARVOX_SFS_PROVIDER");
        if (string.IsNullOrWhiteSpace(root)) root = Path.Combine(runtime, "sfs-provider");
        root = Path.GetFullPath(root!);
        var driver = Path.Combine(root, "Vk3DVision");
        foreach (var name in new[] { "Vk3DVision64.dll", "Vk3DVision64.json", "Spirv-Cross_V1.dll", "Spirv-Cross_V2.dll" })
            if (!File.Exists(Path.Combine(driver, name)))
                throw new FileNotFoundException("Vulkan Single-Frame Stereo needs the prepared local provider. Run tools/prepare_sfs_provider.ps1 first.", Path.Combine(driver, name));
        var profile = Path.Combine(root, "Profiles", "DOOM", "Vk3DVision.ini");
        var text = File.ReadAllText(profile);
        foreach (var setting in new[] { "SingleFrameStereo = true", "Stereo3DViewMode = SBS_LEFT" })
            if (!Regex.IsMatch(text, "(?m)^\\s*" + Regex.Escape(setting).Replace(" ", "\\s*") + "\\s*$"))
                throw new InvalidOperationException("Unsafe SFS provider profile: required " + setting);
        // Outer producer, inner KHARVOX consumer: XR allocations use the
        // downstream dispatch and never pass through the stereorizer.
        start.EnvironmentVariables["Vk3DVision"] = root;
        start.EnvironmentVariables["VK_LAYER_PATH"] = driver + ";" + runtime;
        start.EnvironmentVariables["VK_INSTANCE_LAYERS"] = "VK_LAYER_Vk3DVision;VK_LAYER_KHARVOX_OPENXR";
        start.EnvironmentVariables.Remove("DISABLE_VK_LAYER_Vk3DVision_1");
        start.EnvironmentVariables["KHARVOX_VULKAN_SFS"] = "1";
        start.EnvironmentVariables["DISABLE_VK_LAYER_VALVE_steam_overlay_1"] = "1";
        start.EnvironmentVariables["DISABLE_VK_LAYER_VALVE_steam_fossilize_1"] = "1";
        start.EnvironmentVariables["DISABLE_VULKAN_OBS_CAPTURE"] = "1";
    }
}
