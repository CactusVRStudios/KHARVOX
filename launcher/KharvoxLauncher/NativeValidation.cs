using System.Diagnostics;

namespace KharvoxLauncher;

internal static class NativeValidation
{
    internal static string? Configure(ProcessStartInfo start, bool native, string runtime)
    {
        if (!native || !File.Exists(Path.Combine(runtime, "native_validate_quality"))) return null;
        var layers = Path.Combine(runtime, "validation");
        foreach (var file in new[] { "VkLayer_khronos_validation.dll", "VkLayer_khronos_validation.json" })
            if (!File.Exists(Path.Combine(layers, file))) throw new FileNotFoundException("Native diagnostic layer is missing.", file);
        var log = Path.Combine(Path.GetTempPath(), "KHARVOX-NATIVE-VALIDATION-" + DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N").Substring(0, 8) + ".log");
        File.WriteAllLines(Path.Combine(layers, "vk_layer_settings.txt"), new[] {
            "khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG",
            "khronos_validation.report_flags = error,warn,info",
            "khronos_validation.log_filename = " + log.Replace('\\', '/'),
            "khronos_validation.validate_sync = true",
            "khronos_validation.duplicate_message_limit = 8"
        });
        var previous = start.EnvironmentVariables["VK_ADD_LAYER_PATH"];
        if (string.IsNullOrEmpty(previous) || !previous.Split(';').Contains(layers))
            start.EnvironmentVariables["VK_ADD_LAYER_PATH"] = layers + (string.IsNullOrEmpty(previous) ? "" : ";" + previous);
        previous = start.EnvironmentVariables["VK_INSTANCE_LAYERS"];
        if (string.IsNullOrEmpty(previous) || !previous.Split(';').Contains("VK_LAYER_KHRONOS_validation"))
            start.EnvironmentVariables["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation" + (string.IsNullOrEmpty(previous) ? "" : ";" + previous);
        start.EnvironmentVariables["VK_LAYER_SETTINGS_PATH"] = layers;
        return log;
    }
}
