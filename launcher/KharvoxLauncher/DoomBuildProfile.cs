using System.Security.Cryptography;

namespace KharvoxLauncher;

internal enum DoomDistribution { Steam, Gog }

internal static class DoomBuildProfile
{
    internal const string SteamSha256 = "3d8b4393649f80bdae85c19ae2c985c45ec9ac54f74ab04a18816e30ead63c21";
    internal const string GogSha256 = "a32df8ffa042090f14fe0a200f1c5d7dddf9c947fac223916c252f826f1ecf11";

    internal static DoomDistribution? Identify(string hash) => hash.ToLowerInvariant() switch
    {
        SteamSha256 => DoomDistribution.Steam,
        GogSha256 => DoomDistribution.Gog,
        _ => null
    };

    internal static DoomDistribution Inspect(string executable)
    {
        using var stream = File.OpenRead(executable);
        using var sha = SHA256.Create();
        var hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "").ToLowerInvariant();
        return Identify(hash) ?? throw new InvalidOperationException(
            "Unsupported DOOM executable. KHARVOX 1.4 supports the verified Steam (2024-03-21) " +
            "and experimental GOG (2025-02-25) Vulkan builds. No game hooks were installed.\nSHA256: " + hash);
    }
}
