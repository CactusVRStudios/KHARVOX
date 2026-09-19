namespace KharvoxLauncher;

internal static class RendererSelection
{
    internal static string Normalize(string? key) => key?.ToUpperInvariant() switch
    {
        "AER" => "AER",
        _ => VulkanSfs.Key
    };
    internal static bool IsSfs(string? key) => Normalize(key) == VulkanSfs.Key;
    internal static int Index(string? key) => IsSfs(key) ? 1 : 0;
    // Retained for legacy recovery diagnostics; normalized launch options cannot select it.
    internal static bool IsNative(string? key) => key == "NATIVE";
    internal static readonly string[] ObsoleteMarkers = {
        "native_test_multiview", "native_test_multiview_visible", "native_test_multiview_hybrid",
        "native_test_bind_elision", "enable_nvidia_afw", "enable_native_stereo", "enable_native_two_view",
        "native_test_left_eye_only"
    };
    internal static void ClearObsoleteMarkers(string runtime)
    {
        foreach (var marker in ObsoleteMarkers) File.Delete(Path.Combine(runtime, marker));
    }
}
