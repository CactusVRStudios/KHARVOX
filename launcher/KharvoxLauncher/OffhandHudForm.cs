using System.Globalization;

namespace KharvoxLauncher;

internal sealed class OffhandHudForm : Form
{
    private readonly NumericUpDown[,] fields = new NumericUpDown[2,7];
    private readonly CheckBox enabled = new() { Text = "Bind health and ammo to offhand", Checked = true, AutoSize = true };
    private readonly Label feedback = new() { AutoSize = true, Text = "Apply updates the running game within about half a second." };
    private readonly string path;
    internal OffhandHudForm(string? runtime = null)
    {
        path = Path.Combine(runtime ?? AppContext.BaseDirectory, "offhand_hud.cfg");
        Text = "Offhand HUD calibration (Test)";
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(650,570); MinimumSize = new Size(660,600);
        StartPosition = FormStartPosition.CenterParent;
        var root = new TableLayoutPanel { Dock=DockStyle.Fill, Padding=new Padding(14), ColumnCount=1, RowCount=5 };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute,32));
        root.RowStyles.Add(new RowStyle(SizeType.Percent,100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute,140));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute,40));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute,36));
        Controls.Add(root);root.Controls.Add(enabled);
        var grid = new TableLayoutPanel { Dock=DockStyle.Fill,ColumnCount=3,RowCount=8 };
        for(int c=0;c<3;++c)grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100f/3));
        grid.Controls.Add(new Label { Text="Hand-local adjustment",AutoSize=true },0,0);
        grid.Controls.Add(new Label { Text="Normal: LEFT offhand",AutoSize=true },1,0);
        grid.Controls.Add(new Label { Text="Left Mode: RIGHT offhand",AutoSize=true },2,0);
        string[] labels={"Forward (cm)","Left (cm)","Up (cm)","Pitch (degrees)","Yaw (degrees)","Roll (degrees)","Size (%)"};
        decimal[] defaults={8,0,8,0,0,0,10};
        for(int r=0;r<7;++r){
            grid.RowStyles.Add(new RowStyle(SizeType.Percent,12.5f));
            grid.Controls.Add(new Label { Text=labels[r],AutoSize=true,Anchor=AnchorStyles.Left },0,r+1);
            for(int mode=0;mode<2;++mode){
                var n=new NumericUpDown { Minimum=r==6?2:r<3?-100:-180, Maximum=r==6?200:r<3?100:180,
                    DecimalPlaces=1,Increment=r<3?.1m:1m,Value=defaults[r],Dock=DockStyle.Fill };
                fields[mode,r]=n;grid.Controls.Add(n,mode+1,r+1);
            }
        }
        root.Controls.Add(grid);
        root.Controls.Add(new Label { Dock=DockStyle.Fill,Text="INGAME: Hold ALT for all HUD calibration keys (Num Lock ON).\nNum +: switch rotation / position (starts in rotation).\n4/6: yaw or left/right; 2/8: pitch or down/up; 7/9: roll or forward/back.\nShift: fine steps. Num * / Num /: size up/down. Num 5: reset active handedness.\nNum 0: toggle offhand HUD. Every change is saved automatically.\nNormal mode and Left Mode retain separate calibration." });
        var buttons=new FlowLayoutPanel { Dock=DockStyle.Fill };
        var apply=new Button { Text="Apply live",AutoSize=true };apply.Click+=(_,_)=>Save();buttons.Controls.Add(apply);
        var reset=new Button { Text="Reset both",AutoSize=true };reset.Click+=(_,_)=>{for(int m=0;m<2;++m)for(int r=0;r<7;++r)fields[m,r].Value=defaults[r];};buttons.Controls.Add(reset);
        var close=new Button { Text="Close",AutoSize=true };close.Click+=(_,_)=>Close();buttons.Controls.Add(close);
        root.Controls.Add(buttons);root.Controls.Add(feedback);
        LoadValues();
    }
    private void LoadValues()
    {
        if(!File.Exists(path))return;
        try{
            var words=File.ReadAllText(path).Split((char[]?)null,StringSplitOptions.RemoveEmptyEntries);
            if(words.Length!=16||words[0]!="1"||(words[1]!="0"&&words[1]!="1"))throw new FormatException();
            var values=new decimal[2,7];
            for(int m=0;m<2;++m)for(int r=0;r<7;++r){
                var v=decimal.Parse(words[2+m*7+r],CultureInfo.InvariantCulture)*(r==6?100:1);
                if(v<fields[m,r].Minimum||v>fields[m,r].Maximum)throw new FormatException();values[m,r]=v;
            }
            enabled.Checked=words[1]=="1";
            for(int m=0;m<2;++m)for(int r=0;r<7;++r)fields[m,r].Value=values[m,r];
        }catch(Exception ex){feedback.Text="Could not load calibration: "+ex.Message;}
    }
    private void Save()
    {
        try{
            var lines=new List<string>{"1 "+(enabled.Checked?"1":"0")};
            for(int m=0;m<2;++m){var row=new List<string>();for(int r=0;r<7;++r)
                row.Add((fields[m,r].Value/(r==6?100:1)).ToString(CultureInfo.InvariantCulture));lines.Add(string.Join(" ",row));}
            var temp=path+".tmp";File.WriteAllLines(temp,lines);
            if(File.Exists(path))File.Replace(temp,path,null);else File.Move(temp,path);
            feedback.Text="Saved. Return focus to DOOM to check the placement.";
        }catch(Exception ex){feedback.Text="Save failed: "+ex.Message;}
    }
}
