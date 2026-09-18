using System.Diagnostics;
using System.Security.Cryptography;

namespace KharvoxLauncher;

// KHARVOX's own multiview implementation. The original provider is used only
// by separate reverse-engineering probes, never by this launcher selection.
internal static class VulkanSfs
{
    internal const string Key = "VULKAN_SFS";
    internal static string Label => File.Exists(Path.Combine(AppContext.BaseDirectory, "sfs_source_ring"))
        ? "Vulkan SFS Source Ring (Test)" : "Vulkan Single-Frame Stereo (Test)";
    internal static string Description => File.Exists(Path.Combine(AppContext.BaseDirectory, "sfs_source_ring"))
        ? "Vulkan multiview test for NVIDIA and AMD: stereo images go directly to OpenXR. AMD hardware validation is pending. The desktop window stays black; use the headset for menus."
        : "Experimental same-frame stereo. Headset tracking, hand timing and full game compatibility are still being tested.";
    internal const string Blocker = "This package does not contain a complete native Vulkan SFS test build. Use the prepared 0.96 native test package or select AER.";

    internal static void EnsureAvailable(string? runtime = null)
    {
        runtime ??= AppContext.BaseDirectory;
        var layer = Path.Combine(runtime, "KharvoxLayer.dll");
        var stamp = Path.Combine(runtime, "native_sfs_build.txt");
        if (!File.Exists(layer) || !File.Exists(stamp)) throw new InvalidOperationException(Blocker);
        var lines = File.ReadAllLines(stamp);
        using var sha = SHA256.Create();
        using var stream = File.OpenRead(layer);
        var hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
        if (lines.Length != 2 || lines[0] != "KHARVOX_NATIVE_SFS_1" ||
            !hash.Equals(lines[1], StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("The SFS build manifest does not match KharvoxLayer.dll. Rebuild or restore the complete SFS test package.");
        foreach (var name in new[] { "SPIRV-Cross.txt", "glslang.txt", "SPIRV-Tools.txt" })
            if (!File.Exists(Path.Combine(runtime, "sfs-compiler-licenses", name)))
                throw new InvalidOperationException("SFS compiler license files are missing from the test package.");
        var profile = Path.Combine(runtime, "sfs-profile");
        if (!Directory.Exists(profile) || Directory.GetFiles(profile, "*.spv").Length != 75)
            throw new InvalidOperationException("The local DOOM SFS profile must contain all 75 compiled shaders (sfs-profile).");
        foreach (var file in Directory.GetFiles(profile, "*.spv"))
        {
            using var shader = new BinaryReader(File.OpenRead(file));
            if (shader.BaseStream.Length < 20 || shader.BaseStream.Length % 4 != 0 || shader.ReadUInt32() != 0x07230203)
                throw new InvalidOperationException("Invalid compiled SFS shader profile: " + Path.GetFileName(file));
        }
    }

    internal static void ClearEnvironment(ProcessStartInfo start)
    {
        foreach (var name in new[] { "KHARVOX_VULKAN_SFS", "KHARVOX_SFS_NATIVE_PROBE",
            "KHARVOX_SFS_NATIVE_VR", "KHARVOX_SFS_PROFILE", "KHARVOX_SFS_CAPTURE_ONCE",
            "KHARVOX_SFS_SOURCE_RING", "KHARVOX_SFS_PROFILE_TIMING" })
            start.EnvironmentVariables.Remove(name);
    }

    internal static void Configure(ProcessStartInfo start, string runtime)
    {
        EnsureAvailable(runtime);
        ClearEnvironment(start);
        start.EnvironmentVariables["KHARVOX_SFS_NATIVE_PROBE"] = "1";
        start.EnvironmentVariables["KHARVOX_SFS_NATIVE_VR"] = "1";
        if (File.Exists(Path.Combine(runtime, "sfs_source_ring")))
            start.EnvironmentVariables["KHARVOX_SFS_SOURCE_RING"] = "1";
        start.EnvironmentVariables["KHARVOX_SFS_PROFILE"] = Path.Combine(runtime, "sfs-profile");
    }
}
