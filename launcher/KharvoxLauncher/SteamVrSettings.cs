using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace KharvoxLauncher;

internal enum SteamVrSettingsUpdate
{
    CreatedFile,
    AddedApplication,
    UpdatedApplication,
    AlreadyConfigured
}

internal static class SteamVrSettings
{
    internal const string KharvoxApplicationKey = "steam.app.379720";
    private const string AsyncReprojectionProperty = "disableAsync";

    internal static string SettingsPathForSteam(string steamExecutable)
    {
        var steamDirectory = Path.GetDirectoryName(Path.GetFullPath(steamExecutable))
            ?? throw new ArgumentException("The Steam executable has no parent directory.", nameof(steamExecutable));
        return Path.Combine(steamDirectory, "config", "steamvr.vrsettings");
    }

    internal static SteamVrSettingsUpdate EnsureKharvoxAsyncReprojection(string settingsPath)
    {
        if (string.IsNullOrWhiteSpace(settingsPath))
            throw new ArgumentException("A SteamVR settings path is required.", nameof(settingsPath));

        if (!File.Exists(settingsPath))
        {
            var created = "{\r\n" +
                "   \"" + KharvoxApplicationKey + "\" : {\r\n" +
                "      \"" + AsyncReprojectionProperty + "\" : false\r\n" +
                "   }\r\n}\r\n";
            ValidateRootObject(created);
            WriteAtomically(settingsPath, created, createBackup: false, writeBom: false);
            return SteamVrSettingsUpdate.CreatedFile;
        }

        var originalBytes = File.ReadAllBytes(settingsPath);
        var hadUtf8Bom = originalBytes.Length >= 3
            && originalBytes[0] == 0xEF && originalBytes[1] == 0xBB && originalBytes[2] == 0xBF;
        var original = File.ReadAllText(settingsPath, Encoding.UTF8);
        ValidateRootObject(original);

        var newline = original.IndexOf("\r\n", StringComparison.Ordinal) >= 0 ? "\r\n" : "\n";
        var appMatch = Regex.Match(original,
            "\\\"" + Regex.Escape(KharvoxApplicationKey) + "\\\"\\s*:\\s*\\{",
            RegexOptions.CultureInvariant);
        string updated;
        SteamVrSettingsUpdate result;

        if (appMatch.Success)
        {
            var appOpenBrace = original.IndexOf('{', appMatch.Index + appMatch.Length - 1);
            var appCloseBrace = FindMatchingBrace(original, appOpenBrace);
            var disableAsyncMatch = Regex.Match(original,
                "\\\"" + AsyncReprojectionProperty + "\\\"\\s*:\\s*(true|false)",
                RegexOptions.IgnoreCase | RegexOptions.CultureInvariant,
                TimeSpan.FromSeconds(1));
            while (disableAsyncMatch.Success && (disableAsyncMatch.Index < appOpenBrace
                || disableAsyncMatch.Index > appCloseBrace))
                disableAsyncMatch = disableAsyncMatch.NextMatch();

            if (disableAsyncMatch.Success)
            {
                var value = disableAsyncMatch.Groups[1];
                if (value.Value.Equals("false", StringComparison.OrdinalIgnoreCase))
                    return SteamVrSettingsUpdate.AlreadyConfigured;
                updated = original.Substring(0, value.Index) + "false"
                    + original.Substring(value.Index + value.Length);
                result = SteamVrSettingsUpdate.UpdatedApplication;
            }
            else
            {
                updated = InsertObjectProperty(original, appOpenBrace, appCloseBrace,
                    AsyncReprojectionProperty, "false", newline, "      ");
                result = SteamVrSettingsUpdate.UpdatedApplication;
            }
        }
        else
        {
            var rootOpenBrace = SkipWhitespace(original, 0);
            var rootCloseBrace = FindMatchingBrace(original, rootOpenBrace);
            var rootIndent = DetectChildIndent(original, rootOpenBrace, rootCloseBrace, "   ");
            var appObject = "{" + newline + rootIndent + "   \"" + AsyncReprojectionProperty
                + "\" : false" + newline + rootIndent + "}";
            updated = InsertObjectProperty(original, rootOpenBrace, rootCloseBrace,
                KharvoxApplicationKey, appObject, newline, rootIndent);
            result = SteamVrSettingsUpdate.AddedApplication;
        }

        ValidateRootObject(updated);
        WriteAtomically(settingsPath, updated, createBackup: true, writeBom: hadUtf8Bom);
        return result;
    }

    private static string InsertObjectProperty(string json, int openBrace, int closeBrace,
        string propertyName, string propertyValue, string newline, string childIndent)
    {
        var contentEnd = closeBrace;
        while (contentEnd > openBrace + 1 && char.IsWhiteSpace(json[contentEnd - 1])) contentEnd--;
        var hasProperties = contentEnd > openBrace + 1;
        var insertion = (hasProperties ? "," : string.Empty) + newline + childIndent
            + "\"" + propertyName + "\" : " + propertyValue;

        if (!hasProperties)
        {
            var parentIndent = childIndent.Length >= 3
                ? childIndent.Substring(0, childIndent.Length - 3)
                : string.Empty;
            insertion += newline + parentIndent;
        }

        return json.Substring(0, contentEnd) + insertion + json.Substring(contentEnd);
    }

    private static string DetectChildIndent(string json, int openBrace, int closeBrace,
        string fallback)
    {
        var body = json.Substring(openBrace + 1, closeBrace - openBrace - 1);
        var match = Regex.Match(body, "(?:\\r?\\n)([ \\t]+)\\\"");
        return match.Success ? match.Groups[1].Value : fallback;
    }

    private static int SkipWhitespace(string value, int index)
    {
        while (index < value.Length && (char.IsWhiteSpace(value[index]) || value[index] == '\uFEFF')) index++;
        if (index >= value.Length || value[index] != '{')
            throw new InvalidDataException("SteamVR settings must contain a JSON object at the root.");
        return index;
    }

    private static int FindMatchingBrace(string json, int openBrace)
    {
        if (openBrace < 0 || openBrace >= json.Length || json[openBrace] != '{')
            throw new InvalidDataException("SteamVR settings contain an invalid object boundary.");

        var depth = 0;
        var inString = false;
        var escaped = false;
        for (var index = openBrace; index < json.Length; index++)
        {
            var character = json[index];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (character == '\\') escaped = true;
                else if (character == '"') inString = false;
                continue;
            }

            if (character == '"') inString = true;
            else if (character == '{') depth++;
            else if (character == '}' && --depth == 0) return index;
        }

        throw new InvalidDataException("SteamVR settings contain an unterminated JSON object.");
    }

    private static void ValidateRootObject(string json)
    {
        try
        {
            var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue, RecursionLimit = 256 };
            if (serializer.DeserializeObject(json) is not Dictionary<string, object>)
                throw new InvalidDataException("SteamVR settings must contain a JSON object at the root.");
        }
        catch (InvalidDataException)
        {
            throw;
        }
        catch (Exception exception)
        {
            throw new InvalidDataException("SteamVR settings are not valid JSON; KHARVOX left them unchanged.", exception);
        }
    }

    private static void WriteAtomically(string settingsPath, string contents,
        bool createBackup, bool writeBom)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(settingsPath))
            ?? throw new InvalidOperationException("The SteamVR settings path has no parent directory.");
        Directory.CreateDirectory(directory);

        if (createBackup)
        {
            var backupPath = settingsPath + ".kharvox-backup";
            if (!File.Exists(backupPath)) File.Copy(settingsPath, backupPath);
        }

        var temporaryPath = Path.Combine(directory,
            ".steamvr.vrsettings.kharvox-" + Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            File.WriteAllText(temporaryPath, contents, new UTF8Encoding(writeBom));
            if (File.Exists(settingsPath)) File.Replace(temporaryPath, settingsPath, null, true);
            else File.Move(temporaryPath, settingsPath);
        }
        finally
        {
            try { if (File.Exists(temporaryPath)) File.Delete(temporaryPath); }
            catch { }
        }
    }
}
