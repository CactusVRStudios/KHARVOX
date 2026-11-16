using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using KharvoxLauncher;

namespace KharvoxDesktop;

internal sealed class LauncherViewModel : INotifyPropertyChanged
{
    private readonly string runtimeDirectory;
    private readonly string settingsPath;
    private readonly bool preview;
    private LauncherSettings settings;
    private Process? backend;
    private string? statusPath, controlPath;
    private string lastStatus = "";
    private string status = "Prêt. Reste debout pour calibrer la hauteur du casque.";
    private string? error;
    private bool busy, running;
    private int page;
    private int languageIndex;
    private string LanguagePath => Path.Combine(runtimeDirectory, "launcher_language.cfg");
    public event PropertyChangedEventHandler? PropertyChanged;

    internal LauncherViewModel(string runtimeDirectory, string settingsPath, bool preview = false)
    {
        this.runtimeDirectory = runtimeDirectory;
        this.settingsPath = settingsPath;
        this.preview = preview;
        languageIndex = preview ? 0 : LauncherLanguage.Read(LanguagePath);
        LauncherLanguage.Apply(languageIndex);
        settings = Defaults();
        if (File.Exists(settingsPath)) settings = LauncherSettingsStore.Load(settingsPath);
        else
        {
            if (File.Exists(LauncherSettingsStore.DefaultPath))
                try { settings = LauncherSettingsStore.Load(LauncherSettingsStore.DefaultPath); } catch { }
            // A separate portable profile makes the requested corrections easy
            // to try, without altering the installed launcher's preferences.
            settings.SettingsVersion = LauncherSettings.CurrentVersion;
            settings.Preset = 3;
            settings.ShowHands = settings.PhysicalCrouch = settings.PhysicalGrenadeThrow = true;
        }
        if (settings.RenderScale < 50) settings.RenderScale = 100;
        if (settings.SmoothSpeed < 150) settings.SmoothSpeed = 230;
        if (settings.SnapAngle < 10) settings.SnapAngle = 45;
        if (string.IsNullOrWhiteSpace(settings.GamePath)) settings.GamePath = DiscoverGame();
    }

    private static LauncherSettings Defaults() => new()
    {
        SettingsVersion = LauncherSettings.CurrentVersion, Preset = 3,
        RendererMode = VulkanSfs.Key, RenderScale = 100, SmoothSpeed = 230,
        SnapAngle = 45, Deadzone = .35m, Intense = true, CinematicFreelook = true,
        OtherCinematicsInQuad = false, VirtualGunstock = true, PhysicalGlorykill = true,
        PhysicalGlorykillSpeed = 2.8m, PhysicalGlorykillHands = 2, BackWeapon = 3,
        ShowHands = true, HandsJump = true, PhysicalCrouch = true, PhysicalGrenadeThrow = true
    };

    private static string DiscoverGame()
    {
        var path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
            "Steam", "steamapps", "common", "DOOM");
        return File.Exists(Path.Combine(path, "DOOMx64vk.exe")) ? path : "";
    }

    private void Notify([CallerMemberName] string? name = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    private void Set<T>(T current, T value, Action<T> assign, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(current, value)) return;
        assign(value); settings.Preset = 3; Notify(name); Notify(nameof(PresetIndex));
        Notify(nameof(RendererLabel));
        if (name is nameof(Hands) or nameof(Crouch) or nameof(Throw))
        {
            Notify(nameof(CorrectionsButtonText)); Notify(nameof(CanEnableCorrections));
        }
    }

    public string[] Languages { get; } = ["Français", "English"];
    public string VersionLabel => "v" + typeof(LauncherViewModel).Assembly.GetName().Version!.ToString(3);
    public string WindowTitle => "DOOM VR PLUS · " + VersionLabel;
    public int LanguageIndex
    {
        get => languageIndex;
        set
        {
            if (value is < 0 or > 1 || value == languageIndex) return;
            languageIndex = value;
            LauncherLanguage.Apply(value);
            foreach (var choices in new[] { Presets, Renderers, TurnModes, MoveModes,
                HandSwapModes, GloryHands, BackWeapons, CalibrationWeapons, WeaponModes, GripModes })
                foreach (var choice in choices) choice.Refresh();
            if (!preview)
                try { File.WriteAllText(LanguagePath, value == 1 ? "en\n" : "fr\n"); }
                catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
                { error = LauncherLanguage.Text("Impossible d’enregistrer la langue.") + " " + exception.Message; }
            Notify(null);
        }
    }
    public LocalizedChoice[] Presets { get; } = LauncherLanguage.Choices("Recommandé", "Confort", "Immersif", "Personnalisé");
    public LocalizedChoice[] Renderers { get; } = LauncherLanguage.Choices("SFS · recommandé", "AER · alternatif");
    public LocalizedChoice[] TurnModes { get; } = LauncherLanguage.Choices("Fluide", "Par crans", "Désactivé");
    public LocalizedChoice[] MoveModes { get; } = LauncherLanguage.Choices("Direction du casque", "Direction de la main libre");
    public LocalizedChoice[] HandSwapModes { get; } = LauncherLanguage.Choices("Boutons", "Boutons et sticks");
    public LocalizedChoice[] GloryHands { get; } = LauncherLanguage.Choices("Main gauche", "Main droite", "Les deux mains");
    public LocalizedChoice[] BackWeapons { get; } = LauncherLanguage.Choices("Pistolet", "Fusil de combat", "Fusil plasma", "Fusil d’assaut lourd",
        "Lance-roquettes", "Super Shotgun", "Canon Gauss", "Mitrailleuse", "BFG 9000", "Tronçonneuse");
    public LocalizedChoice[] CalibrationWeapons { get; } = LauncherLanguage.Choices("Pistolet", "Fusil de combat", "Fusil d’assaut lourd", "Fusil plasma",
        "Lance-roquettes", "Super Shotgun", "Canon Gauss", "Mitrailleuse", "BFG 9000", "Tronçonneuse", "Poings", "Fusil d’assaut", "Canon à arc", "Glande de Mancubus");
    public LocalizedChoice[] WeaponModes { get; } = LauncherLanguage.Choices("Jouer", "Calibrer une arme");
    public LocalizedChoice[] GripModes { get; } = LauncherLanguage.Choices("Canon / garde-main", "Prise latérale");

    public int ActivePage { get => page; set { page = value; Notify(null); } }
    public bool PlayPage => page == 0;
    public bool RenderPage => page == 1;
    public bool MovementPage => page == 2;
    public bool HelpPage => page == 3;
    public string PageTitle => page switch { 1 => LauncherLanguage.Text("Rendu & immersion"), 2 => LauncherLanguage.Text("À ta façon de jouer"), 3 => LauncherLanguage.Text("Commandes & diagnostic"), _ => LauncherLanguage.Text("Prêt à retourner sur Mars ?") };
    public string PageSubtitle => page switch { 1 => LauncherLanguage.Text("La netteté et le confort, à ton équilibre."), 2 => LauncherLanguage.Text("Des mouvements naturels. Tes commandes habituelles."), 3 => LauncherLanguage.Text("Les repères utiles, avant de mettre le casque."), _ => LauncherLanguage.Text("DOOM (2016), avec tes mains et tes mouvements.") };
    public string RendererLabel => RendererSelection.IsSfs(settings.RendererMode) ? "SFS" : "AER";
    public bool CanEdit => !busy && !running;
    private bool AllCorrectionsEnabled => Hands && Crouch && Throw;
    public string CorrectionsButtonText => AllCorrectionsEnabled ? LauncherLanguage.Text("3/3 correctifs actifs") : LauncherLanguage.Text("Activer les 3 correctifs");
    public bool CanEnableCorrections => CanEdit && !AllCorrectionsEnabled;
    public bool CanLaunch => !busy;
    public bool Running => running;
    public bool HasError => !string.IsNullOrEmpty(error);
    public string ErrorMessage => LauncherLanguage.Text(error ?? "");
    public string Status => LauncherLanguage.Text(status);
    public string PrimaryButtonText => busy ? LauncherLanguage.Text("INITIALISATION…") : running ? LauncherLanguage.Text("TERMINER LA SESSION") : LauncherLanguage.Text("LANCER EN VR");
    public string StateLabel => preview ? LauncherLanguage.Text("APERÇU") : running ? LauncherLanguage.Text("EN JEU") : busy ? LauncherLanguage.Text("LANCEMENT") : LauncherLanguage.Text("PRÊT");
    public string RuntimeLabel => preview ? LauncherLanguage.Text("INTERFACE AVALONIA") : File.Exists(Path.Combine(runtimeDirectory, LauncherLanguage.Text("KharvoxLayer.dll"))) ? LauncherLanguage.Text("SFS + AER") : LauncherLanguage.Text("PAQUET INCOMPLET");

    public string GamePath { get => settings.GamePath; set => Set(settings.GamePath, value, v => settings.GamePath = v); }
    public int RendererIndex { get => RendererSelection.IsSfs(settings.RendererMode) ? 0 : 1; set { if (value >= 0) Set(settings.RendererMode, value == 0 ? VulkanSfs.Key : "AER", v => settings.RendererMode = v); } }
    public int PresetIndex
    {
        get => settings.Preset;
        set
        {
            if (value < 0 || value > 3 || value == settings.Preset) return;
            if (value != 3)
            {
                var path = settings.GamePath;
                settings = Defaults(); settings.GamePath = path;
                settings.PhysicalCrouch = settings.PhysicalGrenadeThrow = false;
                if (LauncherPresetPolicy.TryGet(value, out var preset)) settings.OtherCinematicsInQuad = preset.RegularCinematicsInCineWindow;
                if (value == 1) { settings.Intense = false; settings.OtherCinematicsInQuad = true; settings.TurnMode = 1; }
            }
            settings.Preset = value; Notify(null);
        }
    }

    public decimal RenderScale { get => settings.RenderScale; set => Set(settings.RenderScale, value, v => settings.RenderScale = v); }
    public decimal TurnSpeed { get => settings.SmoothSpeed; set => Set(settings.SmoothSpeed, value, v => settings.SmoothSpeed = v); }
    public decimal SnapAngle { get => settings.SnapAngle; set => Set(settings.SnapAngle, value, v => settings.SnapAngle = v); }
    public decimal GlorySpeed { get => settings.PhysicalGlorykillSpeed; set => Set(settings.PhysicalGlorykillSpeed, value, v => settings.PhysicalGlorykillSpeed = v); }
    public int TurnMode { get => settings.TurnMode; set => Set(settings.TurnMode, value, v => settings.TurnMode = v); }
    public int MoveMode { get => settings.MovementDirection; set => Set(settings.MovementDirection, value, v => settings.MovementDirection = v); }
    public int HandSwapMode { get => settings.LeftHandSwapMode; set => Set(settings.LeftHandSwapMode, value, v => settings.LeftHandSwapMode = v); }
    public int GloryHand { get => settings.PhysicalGlorykillHands; set => Set(settings.PhysicalGlorykillHands, value, v => settings.PhysicalGlorykillHands = v); }
    public int BackWeapon { get => settings.BackWeapon; set => Set(settings.BackWeapon, value, v => settings.BackWeapon = v); }
    public int WeaponMode { get => settings.WeaponMode; set => Set(settings.WeaponMode, value, v => settings.WeaponMode = v); }
    public int CalibrationWeapon { get => settings.CalibrationWeapon; set => Set(settings.CalibrationWeapon, value, v => settings.CalibrationWeapon = v); }
    public int GripMode { get => settings.GripAlignment; set => Set(settings.GripAlignment, value, v => settings.GripAlignment = v); }
    public bool Hands { get => settings.ShowHands; set => Set(settings.ShowHands, value, v => settings.ShowHands = v); }
    public bool Crouch { get => settings.PhysicalCrouch; set => Set(settings.PhysicalCrouch, value, v => settings.PhysicalCrouch = v); }
    public bool Throw { get => settings.PhysicalGrenadeThrow; set => Set(settings.PhysicalGrenadeThrow, value, v => settings.PhysicalGrenadeThrow = v); }
    public bool Stock { get => settings.VirtualGunstock; set => Set(settings.VirtualGunstock, value, v => settings.VirtualGunstock = v); }
    public bool Laser { get => settings.LaserSight; set => Set(settings.LaserSight, value, v => settings.LaserSight = v); }
    public bool HandsJump { get => settings.HandsJump; set => Set(settings.HandsJump, value, v => settings.HandsJump = v); }
    public bool LeftHanded { get => settings.LeftHanded; set => Set(settings.LeftHanded, value, v => settings.LeftHanded = v); }
    public bool SwapJump { get => settings.SwapJumpCrouch; set => Set(settings.SwapJumpCrouch, value, v => settings.SwapJumpCrouch = v); }
    public bool Immersive { get => settings.Intense; set => Set(settings.Intense, value, v => settings.Intense = v); }
    public bool Freelook { get => settings.CinematicFreelook; set => Set(settings.CinematicFreelook, value, v => settings.CinematicFreelook = v); }
    public bool CineWindow { get => settings.OtherCinematicsInQuad; set => Set(settings.OtherCinematicsInQuad, value, v => settings.OtherCinematicsInQuad = v); }
    public bool FollowWindow { get => settings.CinewindowFollowsHeadset; set => Set(settings.CinewindowFollowsHeadset, value, v => settings.CinewindowFollowsHeadset = v); }
    public bool Fsr { get => settings.UseFsrUpscaling; set => Set(settings.UseFsrUpscaling, value, v => settings.UseFsrUpscaling = v); }
    public bool GloryKill { get => settings.PhysicalGlorykill; set => Set(settings.PhysicalGlorykill, value, v => settings.PhysicalGlorykill = v); }
    public bool Bhaptics { get => settings.EnableBhaptics; set => Set(settings.EnableBhaptics, value, v => settings.EnableBhaptics = v); }
    public bool Psvr2 { get => settings.UsePsvr2Toolkit; set => Set(settings.UsePsvr2Toolkit, value, v => settings.UsePsvr2Toolkit = v); }
    public bool SkipIntro { get => settings.DisableVrIntro; set => Set(settings.DisableVrIntro, value, v => settings.DisableVrIntro = v); }
    public bool ExtendedLog { get => settings.ExtendedLogging; set => Set(settings.ExtendedLogging, value, v => settings.ExtendedLogging = v); }
    public bool DisableAa { get => settings.DisableAa; set => Set(settings.DisableAa, value, v => settings.DisableAa = v); }
    public bool CaptureEyes { get => settings.CaptureEyes; set => Set(settings.CaptureEyes, value, v => settings.CaptureEyes = v); }
    public bool CalibrateHands
    {
        get => settings.HandCalibrationMode != 0;
        set
        {
            Set(settings.HandCalibrationMode, value ? 1 : 0, v => settings.HandCalibrationMode = v);
            if (value && settings.HudDebugging) { settings.HudDebugging = false; Notify(nameof(CalibrateHud)); }
        }
    }
    public bool CalibrateHud
    {
        get => settings.HudDebugging;
        set
        {
            Set(settings.HudDebugging, value, v => settings.HudDebugging = v);
            if (value && settings.HandCalibrationMode != 0) { settings.HandCalibrationMode = 0; Notify(nameof(CalibrateHands)); }
        }
    }

    internal void Save()
    {
        if (preview) return;
        settings.SettingsVersion = LauncherSettings.CurrentVersion;
        LauncherSettingsStore.Save(settingsPath, settings);
    }

    public void EnableCorrections()
    {
        settings.ShowHands = settings.PhysicalCrouch = settings.PhysicalGrenadeThrow = true;
        settings.Preset = 3; Notify(null);
    }

    public void LaunchOrStop()
    {
        if (preview) { status = "Aperçu : aucun jeu n’est lancé."; Notify(nameof(Status)); return; }
        if (backend is not null && !backend.HasExited)
        {
            if (controlPath is not null) File.WriteAllText(controlPath, "stop");
            busy = true; status = "Fermeture de la session…"; Notify(null); return;
        }
        try
        {
            if (!File.Exists(Path.Combine(settings.GamePath, "DOOMx64vk.exe")))
                throw new InvalidOperationException("Choisis le dossier DOOM contenant DOOMx64vk.exe.");
            if (RendererSelection.IsSfs(settings.RendererMode)) VulkanSfs.EnsureAvailable(runtimeDirectory);
            var executable = Path.Combine(runtimeDirectory, "KharvoxLaunchHost.exe");
            if (!File.Exists(executable)) throw new InvalidOperationException("Le paquet est incomplet. Garde tous ses fichiers ensemble.");
            Save();
            var session = Path.Combine(Path.GetTempPath(), "KHARVOX-Desktop", Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(session);
            statusPath = Path.Combine(session, "status.txt"); controlPath = Path.Combine(session, "control.txt");
            var start = new ProcessStartInfo(executable) { UseShellExecute = false, CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = runtimeDirectory };
            foreach (var arg in new[] { "--desktop-run", settingsPath, statusPath, controlPath }) start.ArgumentList.Add(arg);
            backend = Process.Start(start) ?? throw new InvalidOperationException("Impossible de démarrer le lanceur.");
            busy = true; running = false; error = null; lastStatus = "";
            status = "Vérification du jeu et initialisation du casque…"; Notify(null);
        }
        catch (Exception exception) { error = exception.Message; status = "Le lancement nécessite ton attention."; busy = false; Notify(null); }
    }

    public void PollSession()
    {
        if (backend is null) return;
        try
        {
            if (statusPath is not null && File.Exists(statusPath))
            {
                var text = File.ReadAllText(statusPath);
                if (text != lastStatus && text.Contains('\n'))
                {
                    lastStatus = text; var parts = text.Split('\n', 2); status = parts[1];
                    if (parts[0] == "RUNNING") { running = true; busy = false; }
                    if (parts[0] == "ERROR") { error = parts[1]; running = busy = false; }
                    if (parts[0] == "STOPPED") running = busy = false;
                    Notify(null);
                }
            }
            if (backend.HasExited)
            {
                if (backend.ExitCode != 0 && error is null) error = "La session n’a pas démarré. Consulte le journal KHARVOX.";
                running = busy = false; backend.Dispose(); backend = null; Notify(null);
            }
        }
        catch (IOException) { /* Retry while the backend updates its status file. */ }
    }
}
