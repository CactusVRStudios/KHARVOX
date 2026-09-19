using System.Reflection;

namespace KharvoxLauncher;

public sealed class MainForm : Form
{
    private const decimal AerDefaultRenderScale = 100m;
    private const decimal FixedTurnDeadzone = .35m;
    private const int DefaultSmoothTurnSpeed = 230;
    private const decimal GloryKillSpeedMinimum = 1.0m;
    private const decimal GloryKillSpeedStep = .2m;
    private const decimal DefaultGloryKillSpeed = 2.8m;
    private static readonly string[] CalibrationWeaponKeys = [
        "pistol", "shotgun", "heavy_assault_rifle", "plasma_rifle", "rocket_launcher",
        "super_shotgun", "gauss_cannon", "chaingun", "bfg", "chainsaw", "fists",
        "assault_rifle", "arc_cannon", "mancubus_gland"
    ];
    private static readonly string[] CalibrationWeaponNames = [
        "Pistol", "Combat Shotgun", "Heavy Assault Rifle", "Plasma Rifle", "Rocket Launcher",
        "Super Shotgun", "Gauss Cannon", "Chaingun", "BFG 9000", "Chainsaw", "Fists",
        "Assault Rifle", "Arc Cannon", "Mancubus Gland"
    ];
    private static readonly string[] BackWeaponKeys = [
        "pistol", "shotgun", "plasma_rifle", "heavy_assault_rifle",
        "rocket_launcher", "super_shotgun", "gauss_cannon", "chaingun",
        "bfg", "chainsaw"
    ];
    private static readonly string[] BackWeaponNames = [
        "Pistol", "Combat Shotgun", "Plasma Rifle", "Heavy Assault Rifle",
        "Rocket Launcher", "Super Shotgun", "Gauss Cannon", "Chaingun",
        "BFG 9000", "Chainsaw"
    ];
    private static readonly Color PanelColor = Color.FromArgb(30, 30, 33);
    private readonly string SettingsPath;

    private readonly ComboBox preset = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox rendererMode = new() { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
    private readonly ComboBox turnMode = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox movementDirection = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox weaponMode = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox calibrationWeapon = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox gripAlignment = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox leftHandSwapMode = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox physicalGlorykillHands = new() { DropDownStyle = ComboBoxStyle.DropDownList };
    private readonly ComboBox backWeapon = new() { DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill };
    private readonly TextBox doomPath = new() { ReadOnly = true, Dock = DockStyle.Fill };
    private readonly CheckBox intense = MakeCheck("Immersive Mode", false);
    private readonly CheckBox cinematicFreelook = MakeCheck("Freelook in cinematics and Glory Kills", true);
    private readonly CheckBox otherCinematicsInQuad = MakeCheck(
        "Regular cinematics in Cine Window", false);
    private readonly CheckBox cinewindowFollowsHeadset = MakeCheck(
        "Cine Window follows headset", false);
    private readonly CheckBox virtualGunstock = MakeCheck("Virtual Gunstock", false);
    private readonly CheckBox physicalGlorykill = MakeCheck("Physical Glory Kills", false);
    private readonly CheckBox laserSight = MakeCheck("Laser sight", false);
    private readonly CheckBox leftHanded = MakeCheck("Left Hand mode", false);
    private readonly CheckBox hudDebugging = MakeCheck("Enable HUD debugging / calibration", false);
    private readonly CheckBox extendedLogging = MakeCheck("Extended Logging", false);
    private readonly CheckBox captureEyes = MakeCheck("Eye capture: Ctrl+Shift+P (next launch)", false);
    private readonly CheckBox disableAa = MakeCheck("Disable AA (both renderers, next launch)", false);
    private readonly CheckBox handsJump = MakeCheck("Hands Jump", true);
    private readonly CheckBox disableVrIntro = MakeCheck("Disable VR-Intro", false);
    private readonly CheckBox showHands = MakeCheck("Enable Hands", true);
    private readonly CheckBox calibrateHands = MakeCheck("Calibrate hands", false);
    private readonly CheckBox enableBhaptics = MakeCheck("Enable bHaptics", false);
    private readonly CheckBox usePsvr2Toolkit = MakeCheck("Use PSVR2 Toolkit", false);
    private readonly CheckBox useFsrUpscaling = MakeCheck("Use FSR Upscaling", false);
    private readonly Label weaponStatus = new() { AutoSize = false, ForeColor = Color.Silver, TextAlign = ContentAlignment.MiddleLeft };
    private readonly System.Windows.Forms.Timer runtimeStatusTimer = new() { Interval = 500 };
    private readonly NumericUpDown renderScale = MakeNumber(50, decimal.MaxValue, AerDefaultRenderScale, 0, 10);
    private readonly TrackBar smoothSpeed = MakeSlider(150, 400, DefaultSmoothTurnSpeed, 25, 10);
    private readonly Label smoothSpeedValue = MakeSliderValueLabel();
    private readonly NumericUpDown snapAngle = MakeNumber(10, 180, 45, 0);
    private readonly TrackBar physicalGlorykillSpeed = MakeSlider(0, 15, 9, 1, 1);
    private readonly Label physicalGlorykillSpeedValue = MakeSliderValueLabel();
    private readonly Label status = new() { AutoSize = false, TextAlign = ContentAlignment.MiddleCenter, ForeColor = Color.Silver };
    private readonly ToolTip statusToolTip = new();
    private readonly Panel renderScaleHost = new() { Dock = DockStyle.Fill, Margin = Padding.Empty };
    private readonly Button launchButton = new();
    private DevModeForm? devModeForm;
    private CracktroForm? cracktroForm;
    private bool nextCracktroIsAmiga;
    private InfoForm? infoForm;
    private bool launchOperationInProgress;
    private bool lastKnownDoomRunning;
    private bool applyingPreset;
    private readonly Panel viewport = new() { Dock = DockStyle.Fill, AutoScroll = true };
    private TableLayoutPanel? launcherContent;
    private bool fittingWindow;
    private Rectangle fittedWorkArea;

    public MainForm() : this(LauncherSettingsStore.DefaultPath)
    {
        if (!File.Exists(SettingsPath))
            Shown += (_, _) =>
            {
                using var welcome = new WelcomeForm();
                var choice = welcome.ShowDialog(this);
                SaveSettings();
                if (choice == DialogResult.OK) ShowInfo();
            };
    }

    internal MainForm(string settingsPath)
    {
        SettingsPath = settingsPath;
        Text = "KHARVOX Launcher";
        var applicationIcon = System.Drawing.Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        if (applicationIcon is not null) Icon = applicationIcon;
        ClientSize = new Size(548, 876);
        MinimumSize = new Size(280, 240);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.FromArgb(0, 0, 0);
        ForeColor = Color.WhiteSmoke;
        Font = new Font("Segoe UI", 9F);
        AutoScaleMode = AutoScaleMode.Dpi;

        var root = new TableLayoutPanel
        {
            Location = Point.Empty,
            Size = new Size(548, 876),
            MinimumSize = new Size(548, 876),
            Padding = new Padding(18, 14, 18, 10),
            RowCount = 7,
            ColumnCount = 1
        };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 130));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 46));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 330));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 172));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 24));
        launcherContent = root;
        viewport.Controls.Add(root);
        Controls.Add(viewport);
        viewport.ClientSizeChanged += (_, _) => LayoutViewport();

        var banner = new PictureBox { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.Black };
        using (var bannerStream = Assembly.GetExecutingAssembly().GetManifestResourceStream("Kharvox.Branding.Logo"))
        {
            if (bannerStream is not null)
            {
                using var embeddedImage = Image.FromStream(bannerStream);
                banner.Image = new Bitmap(embeddedImage);
            }
        }
        root.Controls.Add(banner);

        var profileRow = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, Padding = new Padding(8, 2, 8, 2) };
        profileRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 112));
        profileRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        profileRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 132));
        profileRow.Controls.Add(new Label
        {
            Text = "Launch profile",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = Color.WhiteSmoke
        });
        preset.Items.AddRange(["Recommended", "Comfort", "Intense", "Custom"]);
        preset.Dock = DockStyle.Fill;
        preset.SelectedIndexChanged += (_, _) => ApplyPreset();
        profileRow.Controls.Add(preset);
        var infoButton = new Button
        {
            Text = "📖 Instructions",
            Dock = DockStyle.Fill,
            Margin = new Padding(3),
            FlatStyle = FlatStyle.Flat,
            BackColor = Color.FromArgb(184, 145, 0),
            ForeColor = Color.Black,
            Font = new Font(Font, FontStyle.Bold),
            AccessibleName = "Open KHARVOX Instructions"
        };
        infoButton.FlatAppearance.BorderColor = Color.FromArgb(212, 172, 24);
        infoButton.FlatAppearance.MouseOverBackColor = Color.FromArgb(205, 164, 12);
        infoButton.FlatAppearance.MouseDownBackColor = Color.FromArgb(160, 125, 0);
        infoButton.Click += (_, _) => ShowInfo();
        profileRow.Controls.Add(infoButton, 2, 0);
        root.Controls.Add(profileRow);

        var installRow = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 3, Padding = new Padding(8, 2, 8, 6) };
        installRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 112));
        installRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        installRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 132));
        installRow.Controls.Add(new Label { Text = "DOOM installation", Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft });
        installRow.Controls.Add(doomPath);
        var browseButton = new Button { Text = "Browse…", Dock = DockStyle.Fill, FlatStyle = FlatStyle.Flat };
        browseButton.Click += BrowseDoomFolder;
        installRow.Controls.Add(browseButton);
        root.Controls.Add(installRow);

        var options = MakeGroup("VR OPTIONS");
        var optionGrid = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(18, 8, 18, 8), RowCount = 11, ColumnCount = 3 };
        optionGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 187));
        optionGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 194));
        optionGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (var i = 0; i < 11; i++) optionGrid.RowStyles.Add(new RowStyle(SizeType.Percent, 100f / 11f));
        rendererMode.Items.AddRange(["AER", VulkanSfs.Label]);
        rendererMode.SelectedIndexChanged += RendererModeChanged;
        renderScale.ValueChanged += OptionChanged;
        var renderingInputWidth = (TextRenderer.MeasureText("Vulkan SFS Source Ring (Test)", Font).Width
            + SystemInformation.VerticalScrollBarWidth + 4) / 2;
        var rendererRow = (TableLayoutPanel)MakeFixedWidth(rendererMode, renderingInputWidth);
        AddField(optionGrid, 0, "Renderer", rendererRow);
        optionGrid.SetColumnSpan(rendererRow, 2);


        optionGrid.Controls.Add(new Label
        {
            Text = "RenderScale (%)",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft
        }, 0, 1);
        var renderScaleRow = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        renderScaleRow.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, renderingInputWidth));
        renderScaleRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        renderScale.Dock = DockStyle.Fill;
        useFsrUpscaling.Dock = DockStyle.Fill;
        useFsrUpscaling.CheckedChanged += OptionChanged;
        renderScaleHost.Margin = renderScale.Margin;
        renderScaleHost.Controls.Add(renderScale);
        renderScaleRow.Controls.Add(renderScaleHost, 0, 0);
        renderScaleRow.Controls.Add(useFsrUpscaling, 1, 0);
        optionGrid.Controls.Add(renderScaleRow, 1, 1);
        optionGrid.SetColumnSpan(renderScaleRow, 2);
        intense.Dock = DockStyle.Fill;
        intense.CheckedChanged += ImmersiveModeChanged;
        optionGrid.Controls.Add(intense, 0, 2);
        optionGrid.SetColumnSpan(intense, 3);
        cinematicFreelook.Dock = DockStyle.Fill;
        cinematicFreelook.Padding = new Padding(22, 0, 0, 0);
        cinematicFreelook.Enabled = false;
        cinematicFreelook.CheckedChanged += OptionChanged;
        optionGrid.Controls.Add(cinematicFreelook, 0, 3);
        optionGrid.SetColumnSpan(cinematicFreelook, 3);
        otherCinematicsInQuad.Dock = DockStyle.Fill;
        otherCinematicsInQuad.Padding = new Padding(22, 0, 0, 0);
        otherCinematicsInQuad.Enabled = false;
        otherCinematicsInQuad.CheckedChanged += OptionChanged;
        optionGrid.Controls.Add(otherCinematicsInQuad, 0, 4);
        optionGrid.SetColumnSpan(otherCinematicsInQuad, 3);
        cinewindowFollowsHeadset.Dock = DockStyle.Fill;
        cinewindowFollowsHeadset.CheckedChanged += OptionChanged;
        optionGrid.Controls.Add(cinewindowFollowsHeadset, 0, 5);
        optionGrid.SetColumnSpan(cinewindowFollowsHeadset, 3);
        physicalGlorykill.Dock = DockStyle.Fill;
        physicalGlorykill.CheckedChanged += PhysicalGlorykillChanged;
        physicalGlorykillSpeed.AccessibleName = "Physical Glory Kill speed";
        physicalGlorykillSpeed.ValueChanged += SpeedSliderChanged;
        optionGrid.Controls.Add(physicalGlorykill, 0, 6);
        var gloryKillSpeedSlider = MakeSpeedSlider(physicalGlorykillSpeed, physicalGlorykillSpeedValue);
        optionGrid.Controls.Add(gloryKillSpeedSlider, 1, 6);
        optionGrid.SetColumnSpan(gloryKillSpeedSlider, 2);
        physicalGlorykillHands.Items.AddRange(["Only Left Hand", "Only Right Hand", "Both Hands"]);
        physicalGlorykillHands.SelectedIndexChanged += OptionChanged;
        AddField(optionGrid, 7, "Glory Kill hands", MakeFixedWidth(physicalGlorykillHands, 126));
        backWeapon.Items.AddRange(BackWeaponNames);
        backWeapon.SelectedIndexChanged += OptionChanged;
        var shoulderAndIntro = new TableLayoutPanel {
            Dock = DockStyle.Fill, Margin = Padding.Empty, Padding = Padding.Empty,
            ColumnCount = 2, RowCount = 1
        };
        shoulderAndIntro.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        shoulderAndIntro.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 140));
        shoulderAndIntro.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        shoulderAndIntro.Controls.Add(MakeFixedWidth(backWeapon, 126), 0, 0);
        AddField(optionGrid, 8, "Shoulder Weapon", shoulderAndIntro);
        optionGrid.SetColumnSpan(shoulderAndIntro, 2);

        virtualGunstock.Dock = DockStyle.Fill;
        virtualGunstock.CheckedChanged += OptionChanged;
        laserSight.Dock = DockStyle.Fill;
        laserSight.CheckedChanged += OptionChanged;
        enableBhaptics.Dock = DockStyle.Fill;
        enableBhaptics.CheckedChanged += OptionChanged;
        usePsvr2Toolkit.Dock = DockStyle.Fill;
        usePsvr2Toolkit.CheckedChanged += OptionChanged;
        optionGrid.Controls.Add(virtualGunstock, 0, 9);
        var sightAndHands = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Margin = Padding.Empty, Padding = Padding.Empty,
            ColumnCount = 2, RowCount = 2
        };
        sightAndHands.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        sightAndHands.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
        sightAndHands.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 140));
        sightAndHands.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 52));
        disableVrIntro.Dock = DockStyle.Fill;
        disableVrIntro.Visible = VrGameIntroSession.HasSeenCurrentRelease;
        disableVrIntro.CheckedChanged += OptionChanged;
        shoulderAndIntro.Controls.Add(disableVrIntro, 1, 0);
        showHands.Dock = DockStyle.Fill;
        sightAndHands.Controls.Add(laserSight, 0, 0);
        sightAndHands.Controls.Add(showHands, 1, 0);
        optionGrid.Controls.Add(sightAndHands, 1, 9);
        optionGrid.SetColumnSpan(sightAndHands, 2);
        optionGrid.SetRowSpan(sightAndHands, 2);
        optionGrid.Controls.Add(enableBhaptics, 0, 10);
        sightAndHands.Controls.Add(usePsvr2Toolkit, 0, 1);
        handsJump.Dock = DockStyle.Fill;
        handsJump.CheckedChanged += OptionChanged;
        statusToolTip.SetToolTip(handsJump, "Raise both controllers upward at 1.9 m/s to jump. Supplements the jump button.");
        sightAndHands.Controls.Add(handsJump, 1, 1);
        options.Controls.Add(optionGrid);
        root.Controls.Add(options);

        var tuning = MakeGroup("MOVEMENT");
        var grid = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(18, 8, 18, 7), RowCount = 4, ColumnCount = 2 };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 187));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (var i = 0; i < 4; i++) grid.RowStyles.Add(new RowStyle(SizeType.Percent, 25));
        turnMode.Items.AddRange(["Smooth", "Snap", "Off"]);
        movementDirection.Items.AddRange(["Head direction", "Off hand direction"]);
        movementDirection.SelectedIndexChanged += OptionChanged;
        leftHandSwapMode.Items.AddRange(["Button swap", "Button and Stick swap"]);
        leftHanded.CheckedChanged += LeftHandedChanged;
        leftHandSwapMode.SelectedIndexChanged += OptionChanged;
        var turnAndMovementDirection = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 2,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        turnAndMovementDirection.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 43));
        turnAndMovementDirection.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 57));
        turnMode.Dock = DockStyle.Fill;
        movementDirection.Dock = DockStyle.Fill;
        turnAndMovementDirection.Controls.Add(turnMode, 0, 0);
        turnAndMovementDirection.Controls.Add(movementDirection, 1, 0);
        AddField(grid, 0, "Turn mode", turnAndMovementDirection);
        smoothSpeed.AccessibleName = "Turn speed";
        smoothSpeed.ValueChanged += SpeedSliderChanged;
        AddField(grid, 1, "Turn speed", MakeSpeedSlider(smoothSpeed, smoothSpeedValue));
        AddField(grid, 2, "Snap angle (°)", MakeFixedWidth(snapAngle, 126));
        leftHanded.Dock = DockStyle.Fill;
        grid.Controls.Add(leftHanded, 0, 3);
        grid.Controls.Add(leftHandSwapMode, 1, 3);
        foreach (Control c in new Control[] { turnMode, snapAngle })
        {
            if (c is ComboBox cb) cb.SelectedIndexChanged += OptionChanged;
            if (c is NumericUpDown n) n.ValueChanged += OptionChanged;
        }
        UpdateSpeedSliderLabels();
        tuning.Controls.Add(grid);
        root.Controls.Add(tuning);

        weaponMode.Items.AddRange(["Gameplay test", "Calibration"]);
        calibrationWeapon.Items.AddRange(CalibrationWeaponNames);
        gripAlignment.Items.AddRange(["Barrel / fore-end", "Side grip"]);
        weaponMode.SelectedIndexChanged += WeaponModeChanged;
        calibrationWeapon.SelectedIndexChanged += WeaponModeChanged;
        gripAlignment.SelectedIndexChanged += WeaponModeChanged;
        hudDebugging.CheckedChanged += OptionChanged;
        extendedLogging.CheckedChanged += OptionChanged;
        disableAa.CheckedChanged += OptionChanged;
        captureEyes.CheckedChanged += OptionChanged;

        showHands.CheckedChanged += (_, _) =>
        {
            if (!showHands.Checked) calibrateHands.Checked = false;
            OptionChanged(showHands, EventArgs.Empty);
        };
        calibrateHands.CheckedChanged += HandCalibrationModeChanged;

        runtimeStatusTimer.Tick += (_, _) => RefreshRuntimeStatus();
        runtimeStatusTimer.Start();

        var action = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            RowCount = 2,
            ColumnCount = 3,
            Padding = new Padding(0, 7, 0, 0),
            BackColor = Color.Transparent
        };
        action.RowStyles.Add(new RowStyle(SizeType.Absolute, 44));
        action.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        action.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 100));
        action.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        action.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 100));
        var bloodArtwork = new OwnedImagePictureBox
        {
            Dock = DockStyle.Fill,
            SizeMode = PictureBoxSizeMode.Zoom,
            BackColor = Color.Transparent,
            Margin = new Padding(0, 0, 10, 0),
            TabStop = false,
            Enabled = false
        };
        using (var artworkStream = Assembly.GetExecutingAssembly()
                   .GetManifestResourceStream("Kharvox.Branding.BloodSplatter"))
        {
            if (artworkStream is not null)
            {
                using var embeddedArtwork = Image.FromStream(artworkStream);
                bloodArtwork.Image = new Bitmap(embeddedArtwork);
            }
        }
        action.Controls.Add(bloodArtwork, 0, 0);
        action.SetRowSpan(bloodArtwork, 2);
        launchButton.Text = "LAUNCH GAME";
        launchButton.Dock = DockStyle.Fill;
        launchButton.FlatStyle = FlatStyle.Flat;
        launchButton.BackColor = Color.Black;
        launchButton.ForeColor = Color.White;
        launchButton.Font = new Font(Font, FontStyle.Bold);
        launchButton.FlatAppearance.BorderColor = Color.DimGray;
        launchButton.Click += LaunchClicked;
        action.Controls.Add(launchButton, 1, 0);
        status.Dock = DockStyle.Fill;
        status.Text = File.Exists(Path.Combine(AppContext.BaseDirectory, "KharvoxLayer.dll")) ? "Ready." : "KharvoxLayer.dll not found.";
        status.Click += FocusRunningGameClicked;
        action.Controls.Add(status, 1, 1);
        root.Controls.Add(action);

        var footerLabel = new Label
        {
            Text = "Made by Cactus",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleRight,
            ForeColor = Color.Gray,
            Cursor = Cursors.Hand
        };
        footerLabel.MouseClick += (_, e) =>
        {
            if (e.Button != MouseButtons.Left) return;
            if ((ModifierKeys & Keys.Shift) != 0) ShowDevMode();
            else
            {
                ShowNextCracktro();
            }
        };
        footerLabel.MouseEnter += (_, _) => footerLabel.ForeColor = Color.Gainsboro;
        footerLabel.MouseLeave += (_, _) => footerLabel.ForeColor = Color.Gray;
        var footer = new TableLayoutPanel {
            Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1,
            Margin = Padding.Empty, Padding = Padding.Empty
        };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        var releaseVersion = VrGameIntroSession.ReleaseVersion;
        var releaseInfo = typeof(MainForm).Assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "";
        footer.Controls.Add(new Label {
            Text = releaseVersion.ToString(releaseVersion.Build == 0 ? 2 : 3)
                + (releaseInfo.Contains("-test") ? " Test" : releaseInfo.Contains("-beta") ? " Beta" : releaseInfo.Contains("-rc") ? " RC" : ""),
            Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = Color.Gray
        }, 0, 0);
        footer.Controls.Add(footerLabel, 1, 0);
        root.Controls.Add(footer);
        LayoutViewport();
        LoadSettings();
        RefreshRenderScaleAvailability();
        if (string.IsNullOrWhiteSpace(doomPath.Text) || !File.Exists(Path.Combine(doomPath.Text, "DOOMx64vk.exe")))
            doomPath.Text = KharvoxRunner.FindDoomInstall() ?? string.Empty;
        RefreshDoomProcessState();
        FormClosing += (_, _) => { runtimeStatusTimer.Stop(); SaveSettings(); };
    }

    protected override void OnLoad(EventArgs e)
    {
        base.OnLoad(e);
        FitWorkingArea(Screen.FromControl(this).WorkingArea);
    }

    protected override void OnLocationChanged(EventArgs e)
    {
        base.OnLocationChanged(e);
        if (Visible && !fittingWindow)
        {
            var work = Screen.FromControl(this).WorkingArea;
            if (work != fittedWorkArea) FitWorkingArea(work);
        }
    }

    protected override void OnDpiChanged(DpiChangedEventArgs e)
    {
        base.OnDpiChanged(e);
        FitWorkingArea(Screen.FromControl(this).WorkingArea);
    }

    internal void FitWorkingArea(Rectangle work)
    {
        if (fittingWindow || work.Width <= 0 || work.Height <= 0) return;
        fittingWindow = true;
        try
        {
            fittedWorkArea = work;
            MinimumSize = new Size(Math.Min(280, work.Width), Math.Min(240, work.Height));
            MaximumSize = work.Size;
            var desiredWidth = Width;
            if (launcherContent is {} content && content.MinimumSize.Height >
                Math.Min(ClientSize.Height, work.Height - (Height - ClientSize.Height)))
                desiredWidth = Math.Max(desiredWidth, content.MinimumSize.Width +
                    (Width - ClientSize.Width) + SystemInformation.VerticalScrollBarWidth);
            var width = Math.Min(desiredWidth, work.Width);
            var height = Math.Min(Height, work.Height);
            Bounds = new Rectangle(
                Math.Max(work.Left, Math.Min(Left, work.Right - width)),
                Math.Max(work.Top, Math.Min(Top, work.Bottom - height)), width, height);
            LayoutViewport();
        }
        finally { fittingWindow = false; }
    }

    private void LayoutViewport()
    {
        if (launcherContent is not { } content) return;
        // Preserve the DPI-scaled content height instead of squeezing rows.
        // Extremely narrow monitors retain horizontal access as well.
        content.Size = new Size(Math.Max(content.MinimumSize.Width, viewport.ClientSize.Width),
            Math.Max(content.MinimumSize.Height, viewport.ClientSize.Height));
    }

    private static CheckBox MakeCheck(string text, bool value) => new() { Text = text, Checked = value, AutoSize = true };

    internal void ApplyRendererStatus(string renderer)
    {
        if (renderer.IndexOf("SFS", StringComparison.OrdinalIgnoreCase) >= 0)
        {
            status.Text = renderer.Trim().Replace("Renderer: ", "");
            var fsrStatus = Path.Combine(AppContext.BaseDirectory, "fsr1_status.txt");
            if (useFsrUpscaling.Checked && File.Exists(fsrStatus)
                && File.ReadAllText(fsrStatus).Contains("FSR1 ACTIVE"))
                status.Text += "; FSR1 active";
        }
        else if (renderer.IndexOf("FSR1 ACTIVE", StringComparison.OrdinalIgnoreCase) >= 0)
            status.Text = "Game running, FSR1 active";
    }

    internal void RefreshRenderScaleAvailability()
    {
        var steamVr = KharvoxRunner.UsesSteamVrRuntime();
        renderScale.Enabled = !steamVr;
        var tip = steamVr ? "Renderscale only works from within SteamVR"
            : "Scales scene and XR resolution in both renderers. 100% scene: 3840 x 2160. No application upper limit; GPU/runtime limits apply. Restart required.";
        statusToolTip.SetToolTip(renderScale, tip);
        statusToolTip.SetToolTip(renderScaleHost, tip);
    }

    private void RefreshRuntimeStatus()
    {
        RefreshRenderScaleAvailability();
        disableVrIntro.Visible = VrGameIntroSession.HasSeenCurrentRelease;
        RefreshDoomProcessState();
        try
        {
            if (!KharvoxRunner.IsRunning)
            {
                UpdateWeaponModeDescription();
                return;
            }
            if (calibrateHands.Checked)
            {
                var handPath = Path.Combine(AppContext.BaseDirectory,
                    "hand_calibration_status.txt");
                if (File.Exists(handPath))
                    weaponStatus.Text = File.ReadAllText(handPath).Trim();
            }
            var weaponPath = Path.Combine(AppContext.BaseDirectory, "two_hand_status.txt");
            if (!calibrateHands.Checked
                && File.Exists(weaponPath))
                weaponStatus.Text = File.ReadAllText(weaponPath).Trim();
            var rendererPath = Path.Combine(AppContext.BaseDirectory, "renderer_status.txt");
            if (File.Exists(rendererPath))
            {
                ApplyRendererStatus(File.ReadAllText(rendererPath));
            }
        }
        catch { /* Live status is optional. */ }
    }

    private void FocusRunningGameClicked(object? sender, EventArgs e)
    {
        if (!KharvoxRunner.IsRunning) return;
        if (!KharvoxRunner.FocusDoom())
            status.Text = "Could not focus DOOM.";
    }

    private string SelectedRendererKey() => rendererMode.SelectedIndex == 0 ? "AER" : VulkanSfs.Key;

    private void RendererModeChanged(object? sender, EventArgs e)
    {
        if (rendererMode.SelectedIndex < 0) return;
        statusToolTip.SetToolTip(rendererMode, rendererMode.SelectedIndex == 1
            ? VulkanSfs.Description : "");
        useFsrUpscaling.Enabled = true;
        RefreshRenderScaleAvailability();
        OptionChanged(sender, e);
    }
    private void WeaponModeChanged(object? sender, EventArgs e) => UpdateWeaponModeDescription();
    private void UpdateWeaponModeDescription()
    {
        var calibrating = weaponMode.SelectedIndex == 1;
        calibrationWeapon.Enabled = calibrating;
        gripAlignment.Enabled = calibrating;
        if (calibrating)
            weaponStatus.Text = $"Equip {SelectedCalibrationWeaponName()}, hold both hands in place, then press {(leftHanded.Checked ? "LEFT" : "RIGHT")} TRIGGER to save its profile. The shot is blocked.";
        else
            weaponStatus.Text = leftHanded.Checked
                ? "Left Hand mode: weapon on the left, support on the right. Saved support points are mirrored automatically."
                : "Every recognized weapon uses its own saved support point and aim alignment. Uncalibrated weapons remain safely one-handed.";
    }

    private string SelectedCalibrationWeaponKey()
    {
        var index = ClampInt(calibrationWeapon.SelectedIndex, 0, CalibrationWeaponKeys.Length - 1);
        return CalibrationWeaponKeys[index];
    }

    private string SelectedCalibrationWeaponName()
    {
        var index = ClampInt(calibrationWeapon.SelectedIndex, 0, CalibrationWeaponNames.Length - 1);
        return CalibrationWeaponNames[index];
    }

    private string SelectedBackWeaponKey()
    {
        var index = ClampInt(backWeapon.SelectedIndex, 0, BackWeaponKeys.Length - 1);
        return BackWeaponKeys[index];
    }
    private static NumericUpDown MakeNumber(decimal min, decimal max, decimal value, int decimals, decimal increment = 1) =>
        new() { Minimum = min, Maximum = max, Value = value, DecimalPlaces = decimals, Increment = increment, Dock = DockStyle.Fill, TextAlign = HorizontalAlignment.Left };
    private static TrackBar MakeSlider(int minimum, int maximum, int value,
        int tickFrequency, int largeChange) => new()
    {
        Minimum = minimum,
        Maximum = maximum,
        Value = value,
        TickFrequency = tickFrequency,
        SmallChange = 1,
        LargeChange = largeChange,
        TickStyle = TickStyle.BottomRight,
        AutoSize = false,
        Dock = DockStyle.Fill,
        Margin = Padding.Empty
    };
    private static Label MakeSliderValueLabel() => new()
    {
        AutoSize = true,
        Anchor = AnchorStyles.Right,
        TextAlign = ContentAlignment.MiddleRight,
        ForeColor = Color.Gainsboro,
        Margin = new Padding(0, 0, 0, 1),
        UseCompatibleTextRendering = true
    };
    private static GroupBox MakeGroup(string text) => new() { Text = text, Dock = DockStyle.Fill, ForeColor = Color.Gainsboro, BackColor = PanelColor, Padding = new Padding(8) };

    private static Control MakeFixedWidth(Control control, int width)
    {
        var host = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, Margin = Padding.Empty };
        host.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, width));
        host.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        control.Dock = DockStyle.Fill;
        host.Controls.Add(control, 0, 0);
        return host;
    }

    private static Control MakeSpeedSlider(TrackBar slider, Label valueLabel)
    {
        var host = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 4,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        host.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 40));
        host.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        host.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 34));
        host.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 58));
        host.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        host.Controls.Add(new Label
        {
            Text = "Slow",
            AutoSize = true,
            Anchor = AnchorStyles.Left,
            TextAlign = ContentAlignment.MiddleLeft,
            ForeColor = Color.Silver,
            Margin = new Padding(0, 0, 0, 1),
            UseCompatibleTextRendering = true
        }, 0, 0);
        host.Controls.Add(slider, 1, 0);
        host.Controls.Add(new Label
        {
            Text = "Fast",
            AutoSize = true,
            Anchor = AnchorStyles.Right,
            TextAlign = ContentAlignment.MiddleRight,
            ForeColor = Color.Silver,
            Margin = new Padding(0, 0, 0, 1),
            UseCompatibleTextRendering = true
        }, 2, 0);
        host.Controls.Add(valueLabel, 3, 0);
        return host;
    }

    private decimal SelectedPhysicalGlorykillSpeed() =>
        GloryKillSpeedMinimum + physicalGlorykillSpeed.Value * GloryKillSpeedStep;

    private void UpdateSpeedSliderLabels()
    {
        smoothSpeedValue.Text = smoothSpeed.Value + "°/s";
        physicalGlorykillSpeedValue.Text = SelectedPhysicalGlorykillSpeed()
            .ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " m/s";
    }

    private void SpeedSliderChanged(object? sender, EventArgs e)
    {
        UpdateSpeedSliderLabels();
        OptionChanged(sender, e);
    }

    private static void AddField(TableLayoutPanel grid, int row, string label, Control control)
    {
        grid.Controls.Add(new Label { Text = label, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, row);
        grid.Controls.Add(control, 1, row);
    }

    private void ApplyPreset()
    {
        if (preset.SelectedIndex < 0 || preset.SelectedIndex == 3) return;
        applyingPreset = true;
        try
        {
            showHands.Checked = LauncherPresetPolicy.DefaultEnableHands;
            handsJump.Checked = true;
            disableAa.Checked = false;
            captureEyes.Checked = false;

            if (LauncherPresetPolicy.TryGet(preset.SelectedIndex, out var defaults))
            {
                rendererMode.SelectedIndex = RendererSelection.Index(defaults.RendererMode);
                renderScale.Value = defaults.RenderScale;
                useFsrUpscaling.Checked = defaults.UseFsrUpscaling;
                intense.Checked = defaults.ImmersiveMode;
                cinematicFreelook.Checked = defaults.CinematicFreelook;
                otherCinematicsInQuad.Checked = defaults.RegularCinematicsInCineWindow;
                cinewindowFollowsHeadset.Checked = defaults.CineWindowFollowsHeadset;
                physicalGlorykill.Checked = defaults.PhysicalGloryKills;
                physicalGlorykillSpeed.Value = GloryKillSpeedToSliderValue(
                    defaults.PhysicalGloryKillSpeed);
                physicalGlorykillHands.SelectedIndex = defaults.GloryKillHands;
                backWeapon.SelectedIndex = defaults.ShoulderWeapon;
                virtualGunstock.Checked = defaults.VirtualGunstock;
                laserSight.Checked = defaults.LaserSight;
                enableBhaptics.Checked = defaults.EnableBhaptics;
                usePsvr2Toolkit.Checked = defaults.UsePsvr2Toolkit;
                turnMode.SelectedIndex = defaults.TurnMode;
                movementDirection.SelectedIndex = defaults.MovementDirection;
                smoothSpeed.Value = defaults.TurnSpeed;
                snapAngle.Value = defaults.SnapAngle;
                leftHanded.Checked = defaults.LeftHandMode;
                leftHandSwapMode.SelectedIndex = defaults.LeftHandSwapMode;
            }
            else
            {
                rendererMode.SelectedIndex = RendererSelection.Index(VulkanSfs.Key);
                renderScale.Value = 100m;
                useFsrUpscaling.Checked = false;
                physicalGlorykill.Checked = true;
                physicalGlorykillSpeed.Value = GloryKillSpeedToSliderValue(DefaultGloryKillSpeed);
                physicalGlorykillHands.SelectedIndex = 2;
                otherCinematicsInQuad.Checked = false;
                cinewindowFollowsHeadset.Checked = false;
                smoothSpeed.Value = DefaultSmoothTurnSpeed;
                snapAngle.Value = 45m;
                intense.Checked = false;
                cinematicFreelook.Checked = false;
                turnMode.SelectedIndex = 1;
                movementDirection.SelectedIndex = 0;
                virtualGunstock.Checked = true;
            }
        }
        finally { applyingPreset = false; }
        cinematicFreelook.Enabled = intense.Checked;
        otherCinematicsInQuad.Enabled = intense.Checked;
        smoothSpeed.Enabled = turnMode.SelectedIndex == 0;
        snapAngle.Enabled = turnMode.SelectedIndex == 1;
    }

    private void OptionChanged(object? sender, EventArgs e)
    {
        if (!applyingPreset && Visible && preset.SelectedIndex >= 0 && preset.SelectedIndex != 3) preset.SelectedIndex = 3;
        smoothSpeed.Enabled = turnMode.SelectedIndex == 0;
        snapAngle.Enabled = turnMode.SelectedIndex == 1;
    }

    private void HandCalibrationModeChanged(object? sender, EventArgs e)
    {
        if (calibrateHands.Checked)
            showHands.Checked = true;
        OptionChanged(sender, e);
    }

    private void ImmersiveModeChanged(object? sender, EventArgs e)
    {
        cinematicFreelook.Enabled = intense.Checked;
        otherCinematicsInQuad.Enabled = intense.Checked;
        OptionChanged(sender, e);
    }

    private void PhysicalGlorykillChanged(object? sender, EventArgs e)
    {
        physicalGlorykillSpeed.Enabled = physicalGlorykill.Checked;
        physicalGlorykillHands.Enabled = physicalGlorykill.Checked;
        OptionChanged(sender, e);
    }

    private void LeftHandedChanged(object? sender, EventArgs e)
    {
        leftHandSwapMode.Enabled = leftHanded.Checked;
        UpdateWeaponModeDescription();
        OptionChanged(sender, e);
    }

    private void ShowNextCracktro()
    {
        cracktroForm?.Close();
        cracktroForm = new CracktroForm(nextCracktroIsAmiga);
        cracktroForm.Show(this);
        cracktroForm.Activate();
        nextCracktroIsAmiga = !nextCracktroIsAmiga;
    }

    private void ShowDevMode()
    {
        devModeForm ??= new DevModeForm(
            weaponMode, calibrationWeapon, gripAlignment, hudDebugging,
            extendedLogging, calibrateHands, weaponStatus, disableAa, captureEyes);
        if (!devModeForm.Visible) devModeForm.Show(this);
        if (devModeForm.WindowState == FormWindowState.Minimized)
            devModeForm.WindowState = FormWindowState.Normal;
        devModeForm.BringToFront();
        devModeForm.Activate();
    }

    private void ShowInfo()
    {
        infoForm ??= new InfoForm();
        if (!infoForm.Visible)
        {
            infoForm.ShowContents();
            infoForm.Show(this);
        }
        if (infoForm.WindowState == FormWindowState.Minimized)
            infoForm.WindowState = FormWindowState.Normal;
        infoForm.BringToFront();
        infoForm.Activate();
    }

    private async void LaunchClicked(object? sender, EventArgs e)
    {
        if (launchOperationInProgress) return;
        if (KharvoxRunner.IsRunning)
        {
            launchOperationInProgress = true;
            launchButton.Enabled = false;
            status.Text = "Ending DOOM …";
            try
            {
                await KharvoxRunner.EndDoomAsync();
                status.Text = "DOOM ended.";
            }
            finally
            {
                launchOperationInProgress = false;
                lastKnownDoomRunning = KharvoxRunner.IsRunning;
                SetRunningState(lastKnownDoomRunning);
                launchButton.Enabled = true;
            }
            return;
        }
        SaveSettings();
        launchOperationInProgress = true;
        launchButton.Enabled = false;
        status.Text = "Launching DOOM …";
        try
        {
            var options = CreateLaunchOptions();
            await KharvoxRunner.LaunchAsync(options, () => {
                SetRunningState(true);
                launchButton.Enabled = true;
                status.Text = "Game running";
            }, message => status.Text = message);
            SetRunningState(KharvoxRunner.IsRunning);
            status.Text = KharvoxRunner.IsRunning ? "Game running" : "DOOM ended.";
        }
        catch (Exception ex)
        {
            try
            {
                File.AppendAllText(Path.Combine(Path.GetTempPath(), "KHARVOX-launcher-errors.log"),
                    DateTime.Now.ToString("O") + Environment.NewLine + ex.ToString() + Environment.NewLine);
            }
            catch { /* Diagnostics must preserve the original error. */ }
            status.Text = "Launch failed.";
            MessageBox.Show(this, ex.Message,
                ex is OtherModsDetectedException ? "KHARVOX - Other mods detected"
                    : ex is HeadsetUnavailableException ? "KHARVOX - Headset not connected" : "KHARVOX - Launch error",
                MessageBoxButtons.OK, ex is HeadsetUnavailableException ? MessageBoxIcon.Information : MessageBoxIcon.Error);
        }
        finally
        {
            launchOperationInProgress = false;
            RefreshDoomProcessState();
            launchButton.Enabled = true;
        }
    }

    internal KharvoxLaunchOptions CreateLaunchOptions() => new(
        intense.Checked,
        intense.Checked && cinematicFreelook.Checked,
        intense.Checked && otherCinematicsInQuad.Checked,
        cinewindowFollowsHeadset.Checked,
        SelectedRendererKey(),
        renderScale.Value,
        useFsrUpscaling.Checked,
        doomPath.Text,
        turnMode.SelectedIndex switch { 1 => "Snap", 2 => "Off", _ => "Smooth" },
        movementDirection.SelectedIndex == 1 ? "off-hand" : "head",
        smoothSpeed.Value,
        snapAngle.Value,
        FixedTurnDeadzone,
        weaponMode.SelectedIndex == 1,
        SelectedCalibrationWeaponKey(),
        gripAlignment.SelectedIndex == 1 ? "side-grip" : "barrel",
        virtualGunstock.Checked,
        physicalGlorykill.Checked,
        SelectedPhysicalGlorykillSpeed(),
        physicalGlorykillHands.SelectedIndex switch { 0 => "left", 1 => "right", _ => "both" },
        leftHanded.Checked,
        leftHandSwapMode.SelectedIndex == 1 ? "buttons-and-sticks" : "buttons",
        laserSight.Checked,
        hudDebugging.Checked,
        extendedLogging.Checked,
        showHands.Checked,
        calibrateHands.Checked ? "rotation" : "off",
        enableBhaptics.Checked,
        usePsvr2Toolkit.Checked,
        SelectedBackWeaponKey(), handsJump.Checked, disableAa.Checked, captureEyes.Checked, disableVrIntro.Checked);

    private void SetRunningState(bool running)
    {
        launchButton.Text = running ? "END GAME" : "LAUNCH GAME";
        launchButton.BackColor = running ? Color.FromArgb(95, 95, 98) : Color.Black;
        status.Cursor = running ? Cursors.Hand : Cursors.Default;
        statusToolTip.SetToolTip(status,
            running ? "Click to bring DOOM to the foreground." : string.Empty);
    }

    private void RefreshDoomProcessState()
    {
        var running = KharvoxRunner.IsRunning;
        var changed = running != lastKnownDoomRunning;
        lastKnownDoomRunning = running;
        SetRunningState(running);
        if (changed && !launchOperationInProgress)
        {
            launchButton.Enabled = true;
            status.Text = running ? "Game running" : "DOOM ended.";
        }
    }

    private void BrowseDoomFolder(object? sender, EventArgs e)
    {
        using var dialog = new FolderBrowserDialog
        {
            Description = "Select the DOOM (2016) folder containing DOOMx64vk.exe",
            ShowNewFolderButton = false,
            SelectedPath = Directory.Exists(doomPath.Text) ? doomPath.Text : string.Empty
        };
        if (dialog.ShowDialog(this) == DialogResult.OK)
        {
            if (!File.Exists(Path.Combine(dialog.SelectedPath, "DOOMx64vk.exe")))
            {
                MessageBox.Show(this, "The selected folder does not contain DOOMx64vk.exe.",
                    "KHARVOX", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            doomPath.Text = dialog.SelectedPath;
            SaveSettings();
        }
    }

    private void LoadSettings()
    {
        try
        {
            var s = LauncherSettingsStore.Load(SettingsPath);
            intense.Checked = s.Intense;
            cinematicFreelook.Checked = s.SettingsVersion < 6 || s.CinematicFreelook;
            otherCinematicsInQuad.Checked = s.SettingsVersion >= 15 && s.OtherCinematicsInQuad;
            cinewindowFollowsHeadset.Checked = s.SettingsVersion >= 16 && s.CinewindowFollowsHeadset;
            doomPath.Text = s.GamePath ?? string.Empty;
            rendererMode.SelectedIndex = RendererSelection.Index(s.RendererMode);
            var defaultScale = AerDefaultRenderScale;
            var savedScale = s.RenderScale;
            renderScale.Value = savedScale >= renderScale.Minimum && savedScale <= renderScale.Maximum
                ? savedScale : defaultScale;
            turnMode.SelectedIndex = ClampInt(s.TurnMode, 0, 2);
            movementDirection.SelectedIndex = s.SettingsVersion >= 24
                ? ClampInt(s.MovementDirection, 0, 1) : 0;
            smoothSpeed.Value = ClampToSlider(s.SmoothSpeed, smoothSpeed);
            snapAngle.Value = Clamp(s.SnapAngle, snapAngle);
            weaponMode.SelectedIndex = ClampInt(s.WeaponMode, 0, 1);
            calibrationWeapon.SelectedIndex = s.SettingsVersion >= 12
                ? ClampInt(s.CalibrationWeapon, 0, CalibrationWeaponKeys.Length - 1) : 1;
            backWeapon.SelectedIndex = s.SettingsVersion >= 19
                ? ClampInt(s.BackWeapon, 0, BackWeaponKeys.Length - 1) : 1;
            gripAlignment.SelectedIndex = ClampInt(s.GripAlignment, 0, 1);
            virtualGunstock.Checked = s.VirtualGunstock;
            physicalGlorykill.Checked = s.SettingsVersion >= 10 && s.PhysicalGlorykill;
            physicalGlorykillSpeed.Value = GloryKillSpeedToSliderValue(s.SettingsVersion >= 11
                ? s.PhysicalGlorykillSpeed : DefaultGloryKillSpeed);
            physicalGlorykillHands.SelectedIndex = s.SettingsVersion >= 11
                ? ClampInt(s.PhysicalGlorykillHands, 0, 2) : 2;
            leftHandSwapMode.SelectedIndex = s.SettingsVersion >= 13
                ? ClampInt(s.LeftHandSwapMode, 0, 1) : 0;
            leftHanded.Checked = s.SettingsVersion >= 13 && s.LeftHanded;
            laserSight.Checked = s.SettingsVersion >= 14 && s.LaserSight;
            hudDebugging.Checked = s.SettingsVersion >= 9 && s.HudDebugging;
            extendedLogging.Checked = s.SettingsVersion >= 25 && s.ExtendedLogging;
            showHands.Checked = s.ShowHands;
            disableVrIntro.Checked = s.DisableVrIntro;
            handsJump.Checked = s.HandsJump;
            disableAa.Checked = s.DisableAa;
            captureEyes.Checked = s.CaptureEyes;

            calibrateHands.Checked = s.SettingsVersion >= 27
                && s.HandCalibrationMode != 0;
            enableBhaptics.Checked = s.SettingsVersion >= 17 && s.EnableBhaptics;
            usePsvr2Toolkit.Checked = s.SettingsVersion >= 21 && s.UsePsvr2Toolkit;
            useFsrUpscaling.Checked = s.SettingsVersion >= 22 && s.UseFsrUpscaling;
            preset.SelectedIndex = ClampInt(s.Preset, 0, 3);
        }
        catch
        {
            rendererMode.SelectedIndex = RendererSelection.Index(VulkanSfs.Key);
            renderScale.Value = AerDefaultRenderScale;
            weaponMode.SelectedIndex = 0;
            calibrationWeapon.SelectedIndex = 1;
            backWeapon.SelectedIndex = 1;
            gripAlignment.SelectedIndex = 0;
            virtualGunstock.Checked = false;
            physicalGlorykill.Checked = false;
            smoothSpeed.Value = DefaultSmoothTurnSpeed;
            movementDirection.SelectedIndex = 0;
            snapAngle.Value = 45m;
            physicalGlorykillSpeed.Value = GloryKillSpeedToSliderValue(DefaultGloryKillSpeed);
            physicalGlorykillHands.SelectedIndex = 2;
            leftHandSwapMode.SelectedIndex = 0;
            leftHanded.Checked = false;
            laserSight.Checked = false;
            hudDebugging.Checked = false;
            extendedLogging.Checked = false;
            showHands.Checked = LauncherPresetPolicy.DefaultEnableHands;
            handsJump.Checked = true;
            disableAa.Checked = false;
            captureEyes.Checked = false;

            calibrateHands.Checked = false;
            enableBhaptics.Checked = false;
            usePsvr2Toolkit.Checked = false;
            useFsrUpscaling.Checked = false;
            cinematicFreelook.Checked = true;
            otherCinematicsInQuad.Checked = false;
            cinewindowFollowsHeadset.Checked = false;
            preset.SelectedIndex = 0;
        }
        finally
        {
            UpdateWeaponModeDescription();
            cinematicFreelook.Enabled = intense.Checked;
            otherCinematicsInQuad.Enabled = intense.Checked;
            physicalGlorykillSpeed.Enabled = physicalGlorykill.Checked;
            physicalGlorykillHands.Enabled = physicalGlorykill.Checked;
            leftHandSwapMode.Enabled = leftHanded.Checked;
            useFsrUpscaling.Enabled = true;
            UpdateSpeedSliderLabels();
        }
    }

    private void SaveSettings()
    {
        try
        {
            var s = new LauncherSettings(preset.SelectedIndex, doomPath.Text, intense.Checked,
                cinematicFreelook.Checked,
                otherCinematicsInQuad.Checked,
                cinewindowFollowsHeadset.Checked,
                SelectedRendererKey(), renderScale.Value, useFsrUpscaling.Checked,
                Math.Max(0, turnMode.SelectedIndex),
                Math.Max(0, movementDirection.SelectedIndex),
                smoothSpeed.Value, snapAngle.Value, FixedTurnDeadzone,
                Math.Max(0, weaponMode.SelectedIndex), Math.Max(0, calibrationWeapon.SelectedIndex),
                Math.Max(0, backWeapon.SelectedIndex),
                Math.Max(0, gripAlignment.SelectedIndex),
                virtualGunstock.Checked, physicalGlorykill.Checked,
                SelectedPhysicalGlorykillSpeed(), Math.Max(0, physicalGlorykillHands.SelectedIndex),
                leftHanded.Checked, Math.Max(0, leftHandSwapMode.SelectedIndex),
                laserSight.Checked,
                hudDebugging.Checked,
                extendedLogging.Checked,
                showHands.Checked,
                calibrateHands.Checked ? 1 : 0,
                enableBhaptics.Checked,
                usePsvr2Toolkit.Checked, handsJump.Checked, disableAa.Checked, captureEyes.Checked);
            s.DisableVrIntro = disableVrIntro.Checked;
            LauncherSettingsStore.Save(SettingsPath, s);
        }
        catch { /* Settings are optional; launching must remain possible. */ }
    }

    private static decimal Clamp(decimal value, NumericUpDown n) => Math.Max(n.Minimum, Math.Min(n.Maximum, value));
    private static int ClampToSlider(decimal value, TrackBar slider) => ClampInt(
        decimal.ToInt32(decimal.Round(value, 0, MidpointRounding.AwayFromZero)),
        slider.Minimum, slider.Maximum);
    private static int GloryKillSpeedToSliderValue(decimal speed)
    {
        var clamped = Math.Max(GloryKillSpeedMinimum,
            Math.Min(GloryKillSpeedMinimum + 15 * GloryKillSpeedStep, speed));
        return decimal.ToInt32(decimal.Round(
            (clamped - GloryKillSpeedMinimum) / GloryKillSpeedStep,
            0, MidpointRounding.AwayFromZero));
    }
    private static int ClampInt(int value, int minimum, int maximum) => Math.Max(minimum, Math.Min(maximum, value));

    private sealed class OwnedImagePictureBox : PictureBox
    {
        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                Image?.Dispose();
                Image = null;
            }
            base.Dispose(disposing);
        }
    }

}
