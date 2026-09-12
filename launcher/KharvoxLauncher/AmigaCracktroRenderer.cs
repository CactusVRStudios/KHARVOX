using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

namespace KharvoxLauncher;

internal sealed class AmigaCracktroRenderer : ICracktroRenderer
{
    private const int Background = unchecked((int)0xff000021);
    private sealed class Sprite
    {
        internal readonly int Width, Height;
        internal readonly int[] Pixels;
        internal Sprite(string name)
        {
            using var stream = CracktroForm.Resource("Amiga." + name);
            using var image = new Bitmap(stream);
            Width = image.Width; Height = image.Height;
            Pixels = new int[Width * Height];
            using var bitmap = new Bitmap(Width, Height, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(bitmap)) g.DrawImageUnscaled(image, 0, 0);
            var data = bitmap.LockBits(new Rectangle(0, 0, Width, Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try { Marshal.Copy(data.Scan0, Pixels, 0, Pixels.Length); }
            finally { bitmap.UnlockBits(data); }
        }
        internal int At(int x, int y) => x >= 0 && y >= 0 && x < Width && y < Height ? Pixels[y * Width + x] : 0;
    }
    private readonly Sprite font = new("font32.png"), copper = new("cop.png"), eq = new("eq.png");
    private readonly Bitmap logo = new(640, 112, PixelFormat.Format32bppArgb);
    private readonly Bitmap scroller = new(640, 32, PixelFormat.Format32bppArgb);
    private readonly Bitmap equalizer = new(528, 100, PixelFormat.Format32bppArgb);
    private readonly int[] history = new int[640 * 32], warped = new int[640 * 32], eqPixels = new int[528 * 100];
    private readonly int[] deformation = new int[640];
    private readonly AmigaAudioEngine audio;
    private int simulationFrame = -1;
    public Bitmap Frame { get; } = new(640, 480, PixelFormat.Format32bppArgb);

    internal AmigaCracktroRenderer() : this(new AmigaAudioEngine()) { }
    internal AmigaCracktroRenderer(AmigaAudioEngine audio)
    {
        this.audio = audio;
        var source = new Sprite("kharvox-amiga.png");
        int left = source.Width, top = source.Height, right = 0, bottom = 0;
        for (int y = 0; y < source.Height; y++) for (int x = 0; x < source.Width; x++)
        {
            int c = source.At(x, y);
            if ((c >> 16 & 255) < 60 && (c >> 8 & 255) < 60) continue;
            left = Math.Min(left, x); right = Math.Max(right, x); top = Math.Min(top, y); bottom = Math.Max(bottom, y);
        }
        var logoPixels = new int[640 * 112];
        for (int y = 0; y < 112; y++) for (int x = 0; x < 640; x++)
        {
            int c = x < 10 || x >= 630 ? Background : source.At(left + (x - 10) * (right - left + 1) / 620, top + y * (bottom - top + 1) / 112);
            logoPixels[y * 640 + x] = (c >> 16 & 255) < 24 && (c >> 8 & 255) < 24 ? Background : c;
        }
        Upload(logo, logoPixels);
        for (int i = 0; i < 640; i++) deformation[i] = 1;
        deformation[0] = deformation[32] = 3;
        deformation[64] = deformation[96] = deformation[128] = 2;
        deformation[415] = deformation[447] = deformation[449] = deformation[478] = deformation[479] = deformation[480] = -1;
        deformation[511] = deformation[543] = deformation[575] = 2;
        deformation[607] = deformation[639] = 3;
    }

    public void Render(double seconds)
    {
        int target = (int)(seconds * 60);
        if (target < simulationFrame) { simulationFrame = -1; Array.Clear(history, 0, history.Length); }
        // A hidden window may miss hours of paints. Twelve seconds of history
        // fully refills this short feedback scroller without an unbounded catch-up.
        if (target - simulationFrame > 720) { simulationFrame = target - 720; Array.Clear(history, 0, history.Length); }
        while (simulationFrame < target) StepScroll(++simulationFrame);
        Upload(scroller, warped);
        using var g = Graphics.FromImage(Frame);
        g.Clear(Color.FromArgb(Background));
        g.InterpolationMode = InterpolationMode.NearestNeighbor;
        g.PixelOffsetMode = PixelOffsetMode.Half;
        for (int strip = 0; strip < 16; strip++)
        {
            double angle = target * .096 + strip * .006;
            int y = (int)(6 + 2 * Math.Sin(angle) + strip * 7 + strip * 6 * Math.Abs(-1 + Math.Sin(angle)));
            g.DrawImage(logo, new Rectangle(0, y, 640, 7), 0, strip * 7, 640, 7, GraphicsUnit.Pixel);
        }
        g.DrawImage(scroller, new Rectangle(40, 310, 570, 32), 0, 0, 570, 32, GraphicsUnit.Pixel);
        RenderEqualizer(target, seconds);
        g.DrawImageUnscaled(equalizer, 52, 360);
        // Intentionally no BRAINWALKER bitmap, credit text or noise overlay.
    }

    private void StepScroll(int frame)
    {
        int travel = (frame + 1) * 3 - 672;
        for (int y = 0; y < 32; y++) for (int x = 0; x < 4; x++)
        {
            int offset = travel + x, color = 0;
            if (offset >= 0 && y < 24)
            {
                int glyph = CracktroText.AmigaScroll[offset / 32 % CracktroText.AmigaScroll.Length] - 32;
                if (glyph >= 0 && glyph < 60) color = font.At(glyph % 20 * 32 + offset % 32, glyph / 20 * 24 + y);
            }
            history[y * 640 + 610 + x] = color;
        }
        Array.Clear(warped, 0, warped.Length);
        int delta = 0;
        for (int x = 0; x < 640; x++)
        {
            int count = deformation[x];
            for (int repeat = 0; repeat < count; repeat++)
            {
                int destination = x + delta + repeat;
                if (destination < 0 || destination >= 640) continue;
                for (int y = 0; y < 32; y++)
                    if ((uint)history[y * 640 + x] >> 24 != 0) warped[y * 640 + destination] = copper.At(destination + 20, y);
            }
            delta += count > 0 ? count - 1 : count;
        }
        Array.Clear(history, 0, history.Length);
        for (int y = 0; y < 32; y++) Array.Copy(warped, y * 640 + 9, history, y * 640, 631);
    }

    private void RenderEqualizer(int frame, double seconds)
    {
        Array.Clear(eqPixels, 0, eqPixels.Length);
        int index = frame % 21, second = frame % 31;
        if (audio.VolumeAt(seconds, 3) > 0) EqTile(index, -36);
        if (seconds % 76.8 > 46) CopyEq(134, 10, 528, 16, .5, 1);
        if (audio.VolumeAt(seconds, 0) > 0) CopyEq(56, 44, 528, 10, .8, 1.2);
        EqTile(index, 0);
        if (second >= 10) EqTile(second - 10, 2);
        if (audio.VolumeAt(seconds, 1) > 0) EqTile(index, 80);
        Upload(equalizer, eqPixels);
    }
    private void EqTile(int index, int top)
    {
        for (int y = 0; y < 48; y++)
        {
            int destination = top + y;
            if (destination < 0 || destination >= 100) continue;
            for (int x = 0; x < 528; x++)
            {
                int color = eq.At(x, index * 48 + y);
                if ((uint)color >> 24 != 0) eqPixels[destination * 528 + x] = color;
            }
        }
    }
    private void CopyEq(int left, int top, int width, int height, double scaleX, double scaleY)
    {
        var copy = (int[])eqPixels.Clone();
        for (int y = 0; y < height * scaleY; y++) for (int x = 0; x < width * scaleX; x++)
        {
            if (left + x >= 528 || top + y >= 100) continue;
            int color = copy[(int)(y / scaleY) * 528 + (int)(x / scaleX)];
            if ((uint)color >> 24 != 0) eqPixels[(top + y) * 528 + left + x] = color;
        }
    }
    private static void Upload(Bitmap bitmap, int[] pixels)
    {
        var data = bitmap.LockBits(new Rectangle(0, 0, bitmap.Width, bitmap.Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        try { Marshal.Copy(pixels, 0, data.Scan0, pixels.Length); }
        finally { bitmap.UnlockBits(data); }
    }
    public void Dispose() { Frame.Dispose(); logo.Dispose(); scroller.Dispose(); equalizer.Dispose(); }
}
