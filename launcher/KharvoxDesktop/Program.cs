using Avalonia;
using Avalonia.Headless;

namespace KharvoxDesktop;

internal static class Program
{
    internal static bool Preview { get; private set; }
    [STAThread]
    public static int Main(string[] args)
    {
        // A separate apphost runs the same launcher/settings for a controlled
        // in-game comparison. The normal executable never enables this mode.
        if (args.Contains("--performance-comparison") || string.Equals(
            Path.GetFileNameWithoutExtension(Environment.ProcessPath),
            "KharvoxPerformance", StringComparison.OrdinalIgnoreCase))
            Environment.SetEnvironmentVariable("KHARVOX_PERFORMANCE_COMPARISON", "1");
        if (args.Contains("--self-test")) return DesktopSelfTest.Run(args);
        if (args.Length >= 2 && args[0] == "--screenshot")
        {
            Preview = true;
            AppBuilder.Configure<App>().UseSkia()
                .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
                .SetupWithoutStarting();
            return DesktopSelfTest.Capture(args[1], args.Length > 2 ? args[2] : "fr");
        }
        Preview = args.Contains("--preview");
        return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }
    public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>()
        .UsePlatformDetect().LogToTrace();
}
