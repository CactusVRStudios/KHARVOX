using Avalonia;
using System.ComponentModel;
using System.Text.Json;

namespace KharvoxDesktop;

internal static class LauncherLanguage
{
    private static readonly Dictionary<string, string> French = Load("fr");
    private static readonly Dictionary<string, string> English = Load("en");
    private static readonly Dictionary<string, string> Keys = French.ToDictionary(p => p.Value, p => p.Key);
    private static bool english;

    private static Dictionary<string, string> Load(string language)
    {
        using var stream = typeof(LauncherLanguage).Assembly.GetManifestResourceStream(
            $"KharvoxDesktop.Localization.{language}.json")
            ?? throw new InvalidDataException("Missing launcher language: " + language);
        return JsonSerializer.Deserialize<Dictionary<string, string>>(stream)
            ?? throw new InvalidDataException("Empty launcher language: " + language);
    }

    internal static int Read(string path)
    {
        try { return File.ReadAllText(path).Trim() == "en" ? 1 : 0; }
        catch (IOException) { return 0; }
        catch (UnauthorizedAccessException) { return 0; }
    }

    internal static void Apply(int index)
    {
        english = index == 1;
        if (Application.Current is { } app)
            foreach (var pair in english ? English : French) app.Resources[pair.Key] = pair.Value;
    }

    internal static string Text(string french) => english && Keys.TryGetValue(french, out var key)
        ? English[key] : french;

    internal static LocalizedChoice[] Choices(params string[] french) =>
        french.Select(value => new LocalizedChoice(value)).ToArray();

    internal static void VerifyCatalog()
    {
        if (!French.Keys.ToHashSet().SetEquals(English.Keys) || English.Values.Any(string.IsNullOrWhiteSpace))
            throw new InvalidDataException("Incomplete launcher translations.");
    }
}

// Labels change on the existing objects, so switching language cannot reset
// ComboBox selections or write a different weapon/renderer index to settings.
public sealed class LocalizedChoice(string french) : INotifyPropertyChanged
{
    public string Label => LauncherLanguage.Text(french);
    public event PropertyChangedEventHandler? PropertyChanged;
    internal void Refresh() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Label)));
    public override string ToString() => Label;
}
