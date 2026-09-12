namespace KharvoxLauncher;

internal sealed class DevModeForm : Form
{
    private static readonly Color PanelColor = Color.FromArgb(30, 30, 33);

    internal DevModeForm(
        ComboBox weaponMode,
        ComboBox calibrationWeapon,
        ComboBox gripAlignment,
        CheckBox hudDebugging,
        CheckBox extendedLogging,
        CheckBox calibrateHands,
        Label weaponStatus, CheckBox? disableAa = null, CheckBox? captureEyes = null)
    {
        Text = "KHARVOX – Dev Mode";
        var applicationIcon = System.Drawing.Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        if (applicationIcon is not null) Icon = applicationIcon;
        ClientSize = new Size(650, 678);
        MinimumSize = new Size(666, 717);
        StartPosition = FormStartPosition.CenterParent;
        BackColor = Color.Black;
        ForeColor = Color.WhiteSmoke;
        Font = new Font("Segoe UI", 9F);
        AutoScaleMode = AutoScaleMode.Dpi;
        ShowInTaskbar = false;

        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(14),
            RowCount = 5,
            ColumnCount = 1
        };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 360));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 26));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        Controls.Add(root);

        var weaponGroup = MakeGroup("WEAPON HANDLING / CALIBRATION");
        var weaponGrid = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(18, 9, 18, 7),
            RowCount = 8,
            ColumnCount = 2
        };
        weaponGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 190));
        weaponGrid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        weaponGrid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        weaponGrid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        weaponGrid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        weaponGrid.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        weaponGrid.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        weaponGrid.RowStyles.Add(new RowStyle(SizeType.Absolute, 34));
        weaponGrid.RowStyles.Add(new RowStyle(SizeType.Absolute, 104));
        weaponGrid.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        AddField(weaponGrid, 0, "Mode", weaponMode);
        AddField(weaponGrid, 1, "Target weapon", calibrationWeapon);
        AddField(weaponGrid, 2, "Aim alignment", gripAlignment);
        hudDebugging.Dock = DockStyle.Fill;
        weaponGrid.Controls.Add(hudDebugging, 0, 3);
        weaponGrid.SetColumnSpan(hudDebugging, 2);
        extendedLogging.Dock = DockStyle.Fill;
        weaponGrid.Controls.Add(extendedLogging, 0, 4);
        weaponGrid.SetColumnSpan(extendedLogging, 2);
        calibrateHands.Dock = DockStyle.Fill;
        weaponGrid.Controls.Add(calibrateHands, 0, 5);
        weaponGrid.SetColumnSpan(calibrateHands, 2);
        var handCalibrationHelp = new Label
        {
            Dock = DockStyle.Fill,
            ForeColor = Color.Silver,
            Text = "Hand calibration follows the equipped weapon and starts on its weapon hand. " +
                "Num + switches rotation / position without restarting. " +
                "Num 0 switches weapon hand / free fist; Num 4/6 = X, " +
                "Num 2/8 = Y, Num 7/9 = Z; Shift = fine step; Num 5 = reset.\n" +
                "Gun position/rotation is saved per weapon and physical hand.",
            TextAlign = ContentAlignment.MiddleLeft
        };
        weaponGrid.Controls.Add(handCalibrationHelp, 0, 6);
        weaponGrid.SetColumnSpan(handCalibrationHelp, 2);
        weaponStatus.Dock = DockStyle.Fill;
        weaponStatus.Padding = new Padding(0, 3, 0, 0);
        weaponGrid.Controls.Add(weaponStatus, 0, 7);
        weaponGrid.SetColumnSpan(weaponStatus, 2);
        weaponGroup.Controls.Add(weaponGrid);
        root.Controls.Add(weaponGroup);
        disableAa ??= new CheckBox { Text = "Disable AA (both renderers, next launch)", AutoSize = true };
        disableAa.Dock = DockStyle.Fill;
        root.Controls.Add(disableAa);
        captureEyes ??= new CheckBox {Text="Eye capture: Ctrl+Shift+P (next launch)",AutoSize=true};
        captureEyes.Dock=DockStyle.Fill;
        root.Controls.Add(captureEyes);
        root.Controls.Add(new Label { Text="Pose trace: Ctrl+Shift+T in game, 20 seconds (requires Extended Logging)", Dock=DockStyle.Fill, AutoSize=true });


        var hotkeyGroup = MakeGroup("HAND / HUD CALIBRATION HOTKEYS");
        var hotkeys = new RichTextBox
        {
            Dock = DockStyle.Fill,
            ReadOnly = true,
            TabStop = false,
            DetectUrls = false,
            BorderStyle = BorderStyle.None,
            BackColor = PanelColor,
            ForeColor = Color.Gainsboro,
            Font = new Font("Consolas", 9F),
            ScrollBars = RichTextBoxScrollBars.Vertical,
            Text =
                "Enable Calibrate hands before launching. Keep DOOM focused.\n\n" +
                "HAND ROTATION / POSITION\n" +
                "  Num +              switch rotation / position (starts in rotation)\n" +
                "  Equipped weapon    selects its own saved gun profile\n" +
                "  Left Hand mode     selects left/right weapon-hand profile\n" +
                "  Num 0              switch weapon hand / global free fist\n" +
                "  Num 4 / 6          X axis - / +\n" +
                "  Num 2 / 8          Y axis - / +\n" +
                "  Num 7 / 9          Z axis - / + (position: farther / closer)\n" +
                "  Shift + adjustment fine step (1 degree / 1 mm)\n" +
                "  Num 5              reset selected hand to defaults\n" +
                "  Every change is saved automatically.\n\n" +
                "Enable HUD debugging / calibration before launching DOOM.\n\n" +
                "GLOBAL HUD\n" +
                "  Num 7 / 9          farther / closer\n" +
                "  Num 4 / 6          move left / right\n" +
                "  Num + / -          increase / decrease size\n" +
                "  Shift + adjustment fine steps\n" +
                "  Num 5              reset preview to launch values\n" +
                "  Num *              save global HUD calibration\n\n" +
                "INDIVIDUAL HUD ELEMENTS / MESSAGES\n" +
                "  Num /              select next recently visible element\n" +
                "  Shift + Num /      select previous element\n" +
                "  Ctrl + Num 4 / 6   move left / right\n" +
                "  Ctrl + Num 8 / 2   move up / down\n" +
                "  Ctrl + Num 7 / 9   farther / closer\n" +
                "  Ctrl + Num 1 / 3   rotate left / right\n" +
                "  Shift + adjustment fine steps\n" +
                "  Ctrl + Num 5       reset selected element\n" +
                "  Ctrl + Num -       exclude / include selected element\n\n" +
                "Individual element changes are saved automatically."
        };
        hotkeyGroup.Padding = new Padding(18, 12, 12, 12);
        hotkeyGroup.Controls.Add(hotkeys);
        root.Controls.Add(hotkeyGroup);

        FormClosing += (_, e) =>
        {
            if (e.CloseReason != CloseReason.UserClosing) return;
            e.Cancel = true;
            Hide();
        };
    }

    private static GroupBox MakeGroup(string text) => new()
    {
        Text = text,
        Dock = DockStyle.Fill,
        ForeColor = Color.Gainsboro,
        BackColor = PanelColor,
        Padding = new Padding(8)
    };

    private static void AddField(TableLayoutPanel grid, int row, string label, Control control)
    {
        grid.Controls.Add(new Label
        {
            Text = label,
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft
        }, 0, row);
        control.Dock = DockStyle.Fill;
        grid.Controls.Add(control, 1, row);
    }
}
