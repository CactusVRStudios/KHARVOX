namespace KharvoxLauncher;

internal static class NativeLaunchRecovery
{
    internal const string RefusalFile = "native_stereo_restart_aer.txt";

    internal static string? DiagnosticFailure(string runtimeDir)
    {
        if ((!File.Exists(Path.Combine(runtimeDir, "native_test_left_eye_only"))
            && !File.Exists(Path.Combine(runtimeDir, "profile_native_gpu_passes"))
            && !File.Exists(Path.Combine(runtimeDir, "debug_native_frame_analysis"))
            && !File.Exists(Path.Combine(runtimeDir, "native_validate_quality")))
            || !File.Exists(Path.Combine(runtimeDir, RefusalFile))) return null;
        return "Native diagnostic stopped: "
            + File.ReadAllText(Path.Combine(runtimeDir, RefusalFile)).Trim()
            + ". AER was not started. See %TEMP%/KHARVOX-NATIVE-STEREO.log.";
    }

    // Call once per user launch, under the launch gate, after excluding a
    // running game. Never call from the automatic process retry loop.
    internal static string? Prepare(string runtimeDir, string rendererMode, string? diagnosticRoot = null)
    {
        var refusal = Path.Combine(runtimeDir, RefusalFile);
        if (!RendererSelection.IsNative(rendererMode)
            || !File.Exists(refusal)) return null;

        diagnosticRoot ??= Path.GetTempPath();
        var archive = Path.Combine(diagnosticRoot, "KHARVOX-native-failures",
            DateTime.UtcNow.ToString("yyyyMMdd-HHmmss", System.Globalization.CultureInfo.InvariantCulture)
            + "-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(archive);
        foreach (var name in new[] { "KHARVOX-NATIVE-STEREO.log", "KHARVOX-NATIVE-FRAME-PACING.log", "renderer_status.txt", "native_multiview_status.txt", "native_cpu_recording_status.txt", "native_multiview_visible_status.txt", "native_hybrid_status.txt", "native_multiview_passes.tsv" })
        {
            var source = name.EndsWith(".log", StringComparison.Ordinal)
                ? Path.Combine(diagnosticRoot, name) : Path.Combine(diagnosticRoot, "KHARVOX-Diagnostics", name);
            if (!File.Exists(source) && !name.EndsWith(".log", StringComparison.Ordinal))
                source = Path.Combine(runtimeDir, name);
            if (File.Exists(source)) File.Copy(source, Path.Combine(archive, name));
        }
        // Move the latch LAST. If evidence cannot be saved, abort the launch
        // with the old refusal still in place. A new refusal remains latched
        // throughout all automatic attempts of this launch.
        File.Move(refusal, Path.Combine(archive, RefusalFile));
        return archive;
    }
}
