namespace KharvoxLauncher;

internal sealed class OtherModsDetectedException : InvalidOperationException
{
    internal OtherModsDetectedException(string gameDirectory, IReadOnlyList<string> entries)
        : base("Other mods detected, please remove other mods to make Doom work."
            + Environment.NewLine + Environment.NewLine + "Detected in: " + gameDirectory
            + Environment.NewLine + string.Join(Environment.NewLine, entries.Select(name => "- " + name)))
    {
    }
}

internal static class ModConflictPreflight
{
    // Executable-directory injection points, not a blanket ban on game DLLs.
    // R.E.A.L. also uses RealRepo / RealConfig beside the game executable:
    // https://www.patreon.com/realvr/posts/how-to-setup-r-e-152405468
    private static readonly HashSet<string> ModFiles = new(StringComparer.OrdinalIgnoreCase)
    {
        "dinput8.dll", "dxgi.dll", "d3d11.dll", "d3d12.dll", "opengl32.dll",
        "version.dll", "winmm.dll", "winhttp.dll", "dsound.dll",
        "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll",
        "RealVR64.dll", "RealVR.dll", "RealVR.ini", "RealConfig.bat"
    };

    internal static string[] FindConflicts(string gameDirectory)
    {
        // Root only: archived/renamed mods and game data are not active loaders.
        // Enumerate on every launch; a cached clean result is not sufficient.
        return new DirectoryInfo(gameDirectory).EnumerateFileSystemInfos()
            .Where(entry => (entry.Attributes & FileAttributes.Directory) != 0
                ? entry.Name.Equals("RealRepo", StringComparison.OrdinalIgnoreCase)
                : ModFiles.Contains(entry.Name)
                    || Path.GetExtension(entry.Name).Equals(".asi", StringComparison.OrdinalIgnoreCase))
            .Select(entry => entry.Name)
            .OrderBy(name => name, StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    internal static void EnsureClean(string gameDirectory)
    {
        string[] conflicts;
        try { conflicts = FindConflicts(gameDirectory); }
        catch (Exception error) when (error is IOException || error is UnauthorizedAccessException)
        {
            throw new InvalidOperationException(
                "Unable to check the DOOM folder for other mods. Check folder access and try again.", error);
        }
        if (conflicts.Length != 0)
            throw new OtherModsDetectedException(gameDirectory, conflicts);
    }
}
