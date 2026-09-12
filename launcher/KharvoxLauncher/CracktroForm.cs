using System.Diagnostics;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Reflection;
using System.Runtime.InteropServices;

namespace KharvoxLauncher;

internal sealed class CracktroForm : Form
{
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 16 };
    private readonly Stopwatch clock = new();
    private readonly ICracktroRenderer renderer;
    private readonly AmigaAudioEngine? amigaAudio;
    private SidAudioPlayer? music;

    internal CracktroForm() : this(false) { }

    internal CracktroForm(bool amiga)
    {
        if (amiga)
        {
            amigaAudio = new AmigaAudioEngine();
            renderer = new AmigaCracktroRenderer(amigaAudio);
        }
        else renderer = new CracktroRenderer();
        Text = amiga ? "KHARVOX — Amiga Cracktro" : "CACTUS VR — Made by Cactus";
        AutoScaleMode = AutoScaleMode.None;
        FormBorderStyle = FormBorderStyle.None;
        StartPosition = FormStartPosition.Manual;
        ClientSize = new Size(800, 600);
        BackColor = Color.Black;
        DoubleBuffered = true;
        KeyPreview = true;
        ShowInTaskbar = false;
        timer.Tick += (_, _) => Invalidate();
    }

    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);
        var screen = Screen.FromControl(Owner ?? this).Bounds;
        Location = new Point(screen.Left + (screen.Width - Width) / 2, screen.Top + (screen.Height - Height) / 2);
        try
        {
            music = new SidAudioPlayer();
            music.Failed += ex =>
            {
                if (IsDisposed || !IsHandleCreated) return;
                try { BeginInvoke(new Action(() =>
                {
                    if (!IsDisposed) MessageBox.Show(this, "Intro playback stopped: " + ex.Message, "KHARVOX", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                })); }
                catch (InvalidOperationException) { /* Window closed while audio was stopping. */ }
            };
            music.Start(amigaAudio is null ? null : amigaAudio.Fill);
        }
        catch (Exception ex)
        {
            music?.Dispose(); music = null;
            MessageBox.Show(this, "Intro playback could not start: " + ex.Message, "KHARVOX", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        clock.Restart();
        timer.Start();
        Activate();
    }

    protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
    {
        if ((keyData & Keys.KeyCode) == Keys.Escape) { Close(); return true; }
        return base.ProcessCmdKey(ref msg, keyData);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        renderer.Render(clock.Elapsed.TotalSeconds);
        e.Graphics.InterpolationMode = InterpolationMode.NearestNeighbor;
        e.Graphics.PixelOffsetMode = PixelOffsetMode.Half;
        var destination = amigaAudio is null ? new Rectangle(80, 100, 640, 400) : new Rectangle(0, 0, 800, 600);
        e.Graphics.DrawImage(renderer.Frame, destination, 0, 0, renderer.Frame.Width, renderer.Frame.Height, GraphicsUnit.Pixel);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { timer.Dispose(); music?.Dispose(); renderer.Dispose(); }
        base.Dispose(disposing);
    }

    internal static Stream Resource(string name) => Assembly.GetExecutingAssembly().GetManifestResourceStream("Kharvox.Cracktro." + name)
        ?? throw new InvalidOperationException("Missing embedded intro resource: " + name);
}

internal interface ICracktroRenderer : IDisposable
{
    Bitmap Frame { get; }
    void Render(double seconds);
}

internal sealed class CracktroRenderer : ICracktroRenderer
{
    private sealed class Sprite
    {
        internal readonly int Width, Height;
        internal readonly int[] Pixels;
        internal Sprite(int width, int height, int[] pixels) { Width = width; Height = height; Pixels = pixels; }
        internal Sprite(string name)
        {
            using var stream = CracktroForm.Resource(name + ".gif");
            using var bitmap = new Bitmap(stream);
            Width = bitmap.Width; Height = bitmap.Height; Pixels = new int[Width * Height];
            for (int y = 0; y < Height; y++) for (int x = 0; x < Width; x++) Pixels[y * Width + x] = bitmap.GetPixel(x, y).ToArgb();
        }
        internal int At(int x, int y) => x >= 0 && y >= 0 && x < Width && y < Height ? Pixels[y * Width + x] : 0;
    }

    private readonly Sprite inside, outside;
    private readonly Sprite innerGradient = new("papillon_inner_gradient"), outerGradient = new("papillon_outer_gradient");
    private readonly Sprite font = new("c64font_papillon"), bigFont = new("c64font_papillon_big");
    private readonly Sprite grid = new("big_font_grid"), bigGradient = new("big_scroller_gradient");
    private readonly int[] pixels = new int[320 * 200];
    public Bitmap Frame { get; } = new(320, 200, PixelFormat.Format32bppArgb);
    private static readonly int[] Red = { 0x68372b, 0x9a6759, 0xb8c76f, 0xffffff, 0xb8c76f, 0x9a6759, 0x68372b };
    private static readonly int[] Brown = { 0x433900, 0x6f4f25, 0xb8c76f, 0xffffff, 0xffffff, 0xb8c76f, 0x6f4f25 };
    private static readonly int[] Grey = { 0x444444, 0x6c6c6c, 0x959595, 0xffffff, 0xffffff, 0x959595, 0x6c6c6c };

    internal CracktroRenderer()
    {
        // The 04b TTF wordmark is rasterized into the embedded bitmap.
        // Preserve its padding and proportions on the 300x56 pixel logo grid.
        using var stream = CracktroForm.Resource("cactus-vr-mask.png");
        using var image = new Bitmap(stream);
        using var source = new Bitmap(image.Width, image.Height, PixelFormat.Format32bppArgb);
        using (var graphics = Graphics.FromImage(source)) graphics.DrawImageUnscaled(image, 0, 0);
        var data = source.LockBits(new Rectangle(0, 0, source.Width, source.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        var pixels = new int[source.Width * source.Height];
        try { Marshal.Copy(data.Scan0, pixels, 0, pixels.Length); }
        finally { source.UnlockBits(data); }
        bool IsWhite(int x, int y) => (pixels[y * source.Width + x] & 255) >= 128;
        int left = source.Width, right = -1, top = source.Height, bottom = -1;
        for (int y = 0; y < source.Height; y++) for (int x = 0; x < source.Width; x++)
        {
            if (!IsWhite(x, y)) continue;
            left = Math.Min(left, x); right = Math.Max(right, x); top = Math.Min(top, y); bottom = Math.Max(bottom, y);
        }
        if (right < left) throw new InvalidDataException("The embedded logo mask is empty.");
        const int width = 300, height = 56;
        var filled = new bool[width * height];
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
            filled[y * width + x] = IsWhite(Math.Min(source.Width - 1, (int)((x + .5) * source.Width / width)),
                Math.Min(source.Height - 1, (int)((y + .5) * source.Height / height)));
        bool Filled(int x, int y) => x >= 0 && y >= 0 && x < width && y < height && filled[y * width + x];
        var fill = new int[width * height];
        var edge = new int[width * height];
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++)
        {
            if (!Filled(x, y)) continue;
            // Separate the one-pixel outline so its raster bars travel independently.
            bool inner = Filled(x - 1, y) && Filled(x + 1, y) && Filled(x, y - 1) && Filled(x, y + 1);
            (inner ? fill : edge)[y * width + x] = unchecked((int)0xffffffff);
        }
        inside = new Sprite(width, height, fill);
        outside = new Sprite(width, height, edge);
    }

    public void Render(double seconds)
    {
        double f = seconds * 60;
        for (int i = 0; i < pixels.Length; i++) pixels[i] = unchecked((int)0xff000000);
        Logo(inside, innerGradient, 10, 0, (int)-Math.Min(64.22, (Math.Floor(f) + 1) * .26));
        Logo(outside, outerGradient, 10, 0, (int)(-100 + (f * .86 % 100.62)));
        Text(font, CracktroText.Tagline, Centered(CracktroText.Tagline), 63, 8);
        Text(font, CracktroText.Presents, Centered(CracktroText.Presents), 95, 8);
        Text(font, CracktroText.Title, Centered(CracktroText.Title), 111, 8);
        Text(font, CracktroText.Date, Centered(CracktroText.Date), 127, 8);
        Text(font, CracktroText.Prompt, Centered(CracktroText.Prompt), 191, 8);
        Scroll(font, CracktroText.UpperScroll, f * .75, 79, 8, 0, 7);
        Tint(0, 123, 320, 10, Red[(int)(f / 5) % 7]);
        Tint(0, 79, 320, 10, Brown[((int)(f / 5) + 3) % 7]);
        for (int i = 0; i < CracktroText.Title.Length; i++)
        {
            int phase = i < CracktroText.Title.Length / 2 ? i % 7 : (CracktroText.Title.Length - 1 - i) % 7;
            Tint(Centered(CracktroText.Title) + i * 8, 110, 8, 7, Grey[((int)(f / 5) + phase) % 7]);
        }
        if (f >= 248) Scroll(bigFont, CracktroText.LowerScroll, (f - 248) * 7, 144, 64, 8, 40);
        int gradientY = (int)(-178 + f * .85 % 72.25);
        for (int y = 0; y < 40; y++) for (int x = 0; x < 320; x++)
        {
            int index = (y + 144) * 320 + x;
            pixels[index] = Darken(pixels[index], bigGradient.At(x, y - gradientY));
            if ((uint)grid.At(x, y) >> 24 != 0) pixels[index] = grid.At(x, y);
        }
        Tint(0, 79, 8, 8, 0); Tint(312, 79, 8, 8, 0);
        var data = Frame.LockBits(new Rectangle(0, 0, 320, 200), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        try { Marshal.Copy(pixels, 0, data.Scan0, pixels.Length); }
        finally { Frame.UnlockBits(data); }
    }

    private static int Centered(string text) => (320 - text.Length * 8) / 2;

    private void Logo(Sprite mask, Sprite gradient, int left, int top, int offset)
    {
        for (int y = 0; y < mask.Height; y++) for (int x = 0; x < mask.Width; x++)
        {
            int m = mask.At(x, y);
            if ((uint)m >> 24 == 0) continue;
            int color = Darken(m, gradient.At(x + left, y + top - offset));
            int i = (y + top) * 320 + x + left;
            int old = pixels[i];
            pixels[i] = unchecked((int)0xff000000) | Math.Min(255, (old >> 16 & 255) + (color >> 16 & 255)) << 16
                | Math.Min(255, (old >> 8 & 255) + (color >> 8 & 255)) << 8 | Math.Min(255, (old & 255) + (color & 255));
        }
    }
    private void Text(Sprite sheet, string text, int left, int top, int width, int sourceTop = 0, int height = 7)
    {
        for (int c = 0; c < text.Length; c++)
        {
            int glyph = text[c] - 33;
            if (glyph < 0) continue;
            for (int y = 0; y < height && top + y < 200; y++) for (int x = 0; x < width; x++)
            {
                int dx = left + c * width + x;
                if (dx < 0 || dx >= 320) continue;
                int color = sheet.At(glyph * width + x, y + sourceTop);
                if ((uint)color >> 24 != 0) pixels[(top + y) * 320 + dx] = color;
            }
        }
    }
    private void Scroll(Sprite sheet, string text, double distance, int top, int width, int sourceTop, int height)
    {
        int period = text.Length * width;
        int left = 320 + width - (int)(distance % period);
        Text(sheet, text, left, top, width, sourceTop, height);
        if (distance >= period) Text(sheet, text, left - period, top, width, sourceTop, height);
    }
    private static int Darken(int a, int b) => unchecked((int)0xff000000) | Math.Min(a >> 16 & 255, b >> 16 & 255) << 16
        | Math.Min(a >> 8 & 255, b >> 8 & 255) << 8 | Math.Min(a & 255, b & 255);
    private void Tint(int left, int top, int width, int height, int color)
    {
        for (int y = top; y < top + height; y++) for (int x = left; x < left + width; x++) pixels[y * 320 + x] = Darken(pixels[y * 320 + x], color);
    }
    public void Dispose() => Frame.Dispose();
}
