using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using SkiaSharp;
using System.Runtime.InteropServices;

namespace KharvoxDesktop;

// Reuse the supplied transparent GIF. Decode once per attachment; only the
// perimeter repaints at the GIF's own cadence, and pauses during gameplay.
public sealed class AnimatedLauncherFrame : Control
{
    private readonly List<WriteableBitmap> frames = new();
    private readonly List<int> durations = new();
    private readonly DispatcherTimer timer = new();
    private int frame;
    internal int FrameCount => frames.Count;

    public AnimatedLauncherFrame()
    {
        timer.Tick += (_, _) =>
        {
            if (!IsEffectivelyVisible || DataContext is LauncherViewModel { Running: true }
                || TopLevel.GetTopLevel(this) is Window { WindowState: WindowState.Minimized }) return;
            frame = (frame + 1) % frames.Count;
            timer.Interval = TimeSpan.FromMilliseconds(durations[frame]);
            InvalidateVisual();
        };
    }

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        using var asset = AssetLoader.Open(new Uri("avares://KharvoxDesktop/Assets/doom-vr-plus-frame.gif"));
        using var stream = new SKManagedStream(asset);
        using var codec = SKCodec.Create(stream) ?? throw new InvalidDataException("Invalid launcher GIF.");
        var info = new SKImageInfo(codec.Info.Width, codec.Info.Height, SKColorType.Bgra8888, SKAlphaType.Premul);
        using var decoded = new SKBitmap(info);
        decoded.Erase(SKColors.Transparent);
        var metadata = codec.FrameInfo;
        for (var index = 0; index < codec.FrameCount; ++index)
        {
            var result = codec.GetPixels(info, decoded.GetPixels(), new SKCodecOptions(index, index - 1));
            if (result != SKCodecResult.Success) throw new InvalidDataException($"Launcher GIF frame {index}: {result}");
            var bitmap = new WriteableBitmap(new PixelSize(info.Width, info.Height), new Vector(96, 96),
                PixelFormat.Bgra8888, AlphaFormat.Premul);
            using (var target = bitmap.Lock())
            {
                var bytes = decoded.Bytes;
                for (var row = 0; row < info.Height; ++row)
                    Marshal.Copy(bytes, row * decoded.RowBytes,
                        IntPtr.Add(target.Address, row * target.RowBytes), info.Width * 4);
            }
            frames.Add(bitmap);
            durations.Add(Math.Max(40, metadata[index].Duration));
        }
        if (frames.Count == 0) throw new InvalidDataException("Empty launcher GIF.");
        frame = 0;
        timer.Interval = TimeSpan.FromMilliseconds(durations[0]);
        timer.Start();
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        timer.Stop();
        foreach (var bitmap in frames) bitmap.Dispose();
        frames.Clear(); durations.Clear(); frame = 0;
        base.OnDetachedFromVisualTree(e);
    }

    public override void Render(DrawingContext context)
    {
        base.Render(context);
        if (frames.Count == 0 || Bounds.Width < 512 || Bounds.Height < 250) return;
        var image = frames[frame];
        var w = Bounds.Width; var h = Bounds.Height;
        void Part(double sx, double sy, double sw, double sh, double x, double y, double width, double height)
        {
            if (width > 0 && height > 0)
                context.DrawImage(image, new Rect(sx, sy, sw, sh), new Rect(x, y, width, height));
        }
        // Separate title and corners keep their original proportions. Only
        // the metal rails stretch; the orange transparent center is unused.
        Part(102, 38, 26, 42, 128, 22, w / 2 - 256, 42);
        Part(384, 38, 26, 42, w / 2 + 128, 22, w / 2 - 256, 42);
        Part(0, 16, 128, 96, 0, 0, 128, 96);
        Part(128, 16, 256, 96, w / 2 - 128, 0, 256, 96);
        Part(384, 16, 128, 96, w - 128, 0, 128, 96);
        Part(0, 112, 64, 266, 0, 96, 64, h - 176);
        Part(448, 112, 64, 266, w - 64, 96, 64, h - 176);
        Part(105, 405, 23, 40, 128, h - 53, w / 2 - 256, 40);
        Part(384, 405, 23, 40, w / 2 + 128, h - 53, w / 2 - 256, 40);
        Part(0, 378, 128, 80, 0, h - 80, 128, 80);
        Part(128, 378, 256, 80, w / 2 - 128, h - 80, 256, 80);
        Part(384, 378, 128, 80, w - 128, h - 80, 128, 80);
    }
}
