using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Avalonia.VisualTree;
using System.Diagnostics;

namespace KharvoxDesktop;

public partial class MainWindow : Window
{
    private readonly LauncherViewModel viewModel;
    private readonly DispatcherTimer timer;

    public MainWindow()
    {
        InitializeComponent();
        viewModel = new LauncherViewModel(AppContext.BaseDirectory,
            Path.Combine(AppContext.BaseDirectory, "KharvoxDesktop.settings.json"), Program.Preview);
        DataContext = viewModel;
        timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(300) };
        timer.Tick += (_, _) => viewModel.PollSession();
        if (!Program.Preview) timer.Start();
        Closed += (_, _) => { timer.Stop(); try { viewModel.Save(); } catch { } };
    }

    private void TitleDrag(object? sender, PointerPressedEventArgs e)
    {
        if (e.GetCurrentPoint(this).Properties.IsLeftButtonPressed
            && e.Source is Avalonia.Visual source && source is not Button and not ComboBox
            && !source.GetVisualAncestors().Any(parent => parent is Button or ComboBox))
            BeginMoveDrag(e);
    }
    private void MinimizeClick(object? sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void MaximizeClick(object? sender, RoutedEventArgs e) => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
    private void CloseClick(object? sender, RoutedEventArgs e) => Close();
    private void LaunchClick(object? sender, RoutedEventArgs e) => viewModel.LaunchOrStop();
    private void EnableCorrectionsClick(object? sender, RoutedEventArgs e) => viewModel.EnableCorrections();

    private async void BrowseClick(object? sender, RoutedEventArgs e)
    {
        var folders = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
            { Title = LauncherLanguage.Text("Choisir le dossier de DOOM (2016)"), AllowMultiple = false });
        var path = folders.FirstOrDefault()?.TryGetLocalPath();
        if (path is not null) viewModel.GamePath = path;
    }
    private void SupportClick(object? sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo("https://ko-fi.com/cactusvrstudios/tip") { UseShellExecute = true });
    private void LogsClick(object? sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo(Path.GetTempPath()) { UseShellExecute = true });
}
