namespace KharvoxLauncher;

internal static class RendererSelection
{
    internal static string Normalize(string? key) => key?.ToUpperInvariant() switch
    {
        "NATIVE" or "NATIVE_MULTIVIEW" or "NATIVE_MULTIVIEW_VISIBLE" or
        "NATIVE_MULTIVIEW_HYBRID" or "NATIVE_CPU_RECORDING" => "NATIVE",
        _ => "AER"
    };
    internal static bool IsNative(string? key) => Normalize(key) == "NATIVE";
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
