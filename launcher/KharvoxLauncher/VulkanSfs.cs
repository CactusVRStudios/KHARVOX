using System.Diagnostics;

namespace KharvoxLauncher;

// KHARVOX's own multiview implementation. The original provider is used only
// by separate reverse-engineering probes, never by this launcher selection.
internal static class VulkanSfs
{
    internal const string Key = "VULKAN_SFS";
    internal const string Label = "Vulkan Single-Frame Stereo (Test)";
    internal const string Blocker = "Vulkan Single-Frame Stereo 0.96 is under development. The native resource core and profile matching are implemented; stereo shader bindings and OpenXR eye transport are not integrated yet. See Docs/VULKAN_SFS_096.md. Use AER for gameplay.";

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
