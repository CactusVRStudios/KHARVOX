namespace KharvoxLauncher;

internal sealed class WelcomeForm : Form
{
    internal WelcomeForm()
    {
        Text = "Welcome to KHARVOX";
        ClientSize = new Size(540, 225);
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        ControlBox = false;
        ShowInTaskbar = false;
        AutoScaleMode = AutoScaleMode.Dpi;
        BackColor = Color.FromArgb(24, 24, 27);
        ForeColor = Color.WhiteSmoke;
        Font = new Font("Segoe UI", 10F);

        var layout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill, Padding = new Padding(24),
            ColumnCount = 1, RowCount = 3
        };
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 45));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        layout.Controls.Add(new Label
        {
            Text = "Welcome to KHARVOX", Dock = DockStyle.Fill,
            Font = new Font("Segoe UI", 17F, FontStyle.Bold)
        });
        layout.Controls.Add(new Label
        {
            Text = "A VR mod for DOOM (2016).\n\nPlease read the instructions carefully before playing, especially the controller bindings and known limitations.",
            Dock = DockStyle.Fill
        });
        var buttons = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2 };
        buttons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        buttons.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        var instructions = new Button
        {
            Text = "Open Instructions", DialogResult = DialogResult.OK,
            Dock = DockStyle.Fill, FlatStyle = FlatStyle.Flat,
            BackColor = Color.FromArgb(184, 145, 0), ForeColor = Color.Black,
            Font = new Font(Font, FontStyle.Bold), Margin = new Padding(0, 0, 8, 0)
        };
        var skip = new Button
        {
            Text = "I know what I'm doing", DialogResult = DialogResult.Cancel,
            Dock = DockStyle.Fill, FlatStyle = FlatStyle.Flat,
            BackColor = Color.Black, ForeColor = Color.WhiteSmoke,
            Margin = new Padding(8, 0, 0, 0)
        };
        buttons.Controls.Add(instructions);
        buttons.Controls.Add(skip);
        layout.Controls.Add(buttons);
        Controls.Add(layout);
        AcceptButton = instructions;
        CancelButton = skip;
    }
}
