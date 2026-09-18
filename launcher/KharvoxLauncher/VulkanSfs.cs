using System.Diagnostics;

namespace KharvoxLauncher;

// KHARVOX's own multiview implementation. The original provider is used only
// by separate reverse-engineering probes, never by this launcher selection.
internal static class VulkanSfs
{
    internal const string Key = "VULKAN_SFS";
    internal const string Label = "Vulkan Single-Frame Stereo (Test)";
    internal const string Blocker = "Vulkan Single-Frame Stereo 0.96 is a developer prototype. Native multiview shaders and OpenXR eye transport are implemented and simulator-tested. Headset tracking, hand timing and full game compatibility still require validation. Use the documented native test procedure in Docs/VULKAN_SFS_096.md, or select AER for normal gameplay.";

    internal static void EnsureAvailable()
    {
        throw new InvalidOperationException(Blocker);
    }

    internal static void Configure(ProcessStartInfo start, string runtime)
    {
        // Also guard direct callers: a partial native path must not launch a
        // mono image under a stereo renderer label.
        EnsureAvailable();
    }
}
