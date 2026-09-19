using System.Diagnostics;
using System.Reflection;

namespace KharvoxLauncher;

internal sealed class InfoForm : Form
{
    private const string ColumnBreakTag = "KHARVOX_MANUAL_COLUMN_BREAK";
    private const string ForcedColumnBreakTag = "KHARVOX_MANUAL_FORCED_COLUMN_BREAK";
    private const string WideArtworkPlaceholderTag = "KHARVOX_MANUAL_WIDE_ARTWORK";
    private const string WideArtworkRightSpacerTag = "KHARVOX_MANUAL_WIDE_ARTWORK_RIGHT_SPACER";
    private const string Flat2VrDiscordUrl =
        "https://discord.com/invite/ZFSCSDe";
    private const string BhapticsPlayerUrl =
        "https://www.bhaptics.com/support/downloads/?type=bhaptics_player";
    private const string Psvr2ToolkitUrl =
        "https://github.com/BnuuySolutions/PSVR2Toolkit";
    private const string Psvr2ToolkitSupportUrl =
        "https://discord.gg/jym8CgJPF3";

    private static readonly Color PanelColor = Color.FromArgb(30, 30, 33);
    private static readonly Color PageColor = Color.FromArgb(22, 22, 24);
    private static readonly Color BookTextSurfaceColor = Color.FromArgb(40, 39, 38);
    private static readonly Color AccentColor = Color.FromArgb(225, 66, 50);
    private static readonly Color MutedColor = Color.FromArgb(170, 170, 174);
    private static readonly string[] PageTitles =
    [
        "Contents",
        "General Information and Limitations",
        "Controller Bindings",
        "Movement",
        "Game Options",
        "Rendering",
        "bHaptics",
        "PSVR2 Toolkit",
        "About"
    ];

    private readonly BookPageHost pageHost = new()
    {
        Dock = DockStyle.Fill,
        BackColor = PageColor,
        BorderStyle = BorderStyle.None,
        Margin = Padding.Empty
    };
    private readonly ManualSpread?[] pages = new ManualSpread?[PageTitles.Length];
    private readonly System.Windows.Forms.Timer resizeSettledTimer = new()
    {
        Interval = 100
    };
    private readonly Button previousButton;
    private readonly Button contentsButton;
    private readonly Button nextButton;
    private readonly Label pageNumber = new()
    {
        Dock = DockStyle.Fill,
        ForeColor = MutedColor,
        TextAlign = ContentAlignment.MiddleCenter
    };
    private int currentPage = -1;

    internal InfoForm()
    {
        Text = "KHARVOX Instructions";
        var applicationIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
        if (applicationIcon is not null) Icon = applicationIcon;
        ClientSize = new Size(980, 720);
        MinimumSize = new Size(760, 600);
        StartPosition = FormStartPosition.CenterParent;
        BackColor = Color.Black;
        ForeColor = Color.WhiteSmoke;
        Font = new Font("Segoe UI", 9.5F);
        AutoScaleMode = AutoScaleMode.Dpi;
        ShowInTaskbar = false;

        using (var bookStream = Assembly.GetExecutingAssembly()
                   .GetManifestResourceStream("Kharvox.Branding.ManualBook"))
        {
            if (bookStream is not null)
            {
                using var embeddedBook = Image.FromStream(bookStream);
                pageHost.BookImage = new Bitmap(embeddedBook);
            }
        }

        var root = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(18, 16, 18, 14),
            RowCount = 3,
            ColumnCount = 1
        };
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 68));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.Absolute, 58));
        Controls.Add(root);

        var header = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 2,
            Margin = Padding.Empty
        };
        header.RowStyles.Add(new RowStyle(SizeType.Absolute, 37));
        header.RowStyles.Add(new RowStyle(SizeType.Absolute, 25));
        header.Controls.Add(new Label
        {
            Text = "KHARVOX INSTRUCTIONS",
            Dock = DockStyle.Fill,
            ForeColor = AccentColor,
            Font = new Font("Segoe UI Semibold", 17F, FontStyle.Bold),
            TextAlign = ContentAlignment.BottomLeft
        });
        header.Controls.Add(new Label
        {
            Text = "DOOM (2016) VR conversion — controls, options and compatibility",
            Dock = DockStyle.Fill,
            ForeColor = MutedColor,
            TextAlign = ContentAlignment.TopLeft
        });
        root.Controls.Add(header, 0, 0);
        root.Controls.Add(pageHost, 0, 1);

        var footer = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 5,
            RowCount = 1,
            Padding = new Padding(0, 12, 0, 0),
            Margin = Padding.Empty
        };
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 112));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 152));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 132));
        footer.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 132));
        previousButton = MakeNavigationButton("‹  PREVIOUS");
        contentsButton = MakeNavigationButton("CONTENTS");
        nextButton = MakeNavigationButton("NEXT  ›");
        previousButton.Click += (_, _) => ShowPage(currentPage - 1);
        contentsButton.Click += (_, _) => ShowPage(0);
        nextButton.Click += (_, _) => ShowPage(currentPage + 1);
        footer.Controls.Add(contentsButton, 0, 0);
        footer.Controls.Add(pageNumber, 2, 0);
        footer.Controls.Add(previousButton, 3, 0);
        footer.Controls.Add(nextButton, 4, 0);
        root.Controls.Add(footer, 0, 2);

        BuildManualPages();
        ShowPage(0);
        resizeSettledTimer.Tick += (_, _) => RefreshCurrentPageAfterResize();
        SizeChanged += (_, _) => QueueResizeRefresh();
        FormClosing += (_, e) =>
        {
            if (e.CloseReason != CloseReason.UserClosing) return;
            e.Cancel = true;
            Hide();
        };
    }

    internal void ShowContents() => ShowPage(0);

    internal static string[] PageTitlesForTest() => PageTitles.ToArray();

    internal static string[] ExternalLinksForTest() =>
    [
        Flat2VrDiscordUrl,
        BhapticsPlayerUrl,
        Psvr2ToolkitUrl,
        Psvr2ToolkitSupportUrl
    ];

    private void QueueResizeRefresh()
    {
        resizeSettledTimer.Stop();
        resizeSettledTimer.Start();
    }

    private void RefreshCurrentPageAfterResize()
    {
        resizeSettledTimer.Stop();
        if (currentPage < 0 || pages[currentPage] is not { } page ||
            pageHost.ClientSize.Width <= 0 || pageHost.ClientSize.Height <= 0)
            return;

        pageHost.SuspendLayout();
        page.Bounds = pageHost.ClientRectangle;
        page.RefreshSpreadLayout();
        page.Visible = true;
        page.BringToFront();
        pageHost.ResumeLayout(true);
        pageHost.Invalidate(true);
        page.Invalidate(true);
    }

    private void ShowPage(int pageIndex)
    {
        var nextPage = Math.Max(0, Math.Min(PageTitles.Length - 1, pageIndex));
        pageHost.SuspendLayout();
        if (currentPage >= 0 && pages[currentPage] is { } oldPage)
            oldPage.Visible = false;
        currentPage = nextPage;
        if (pages[currentPage] is { } page)
        {
            page.Bounds = pageHost.ClientRectangle;
            page.Visible = true;
            page.BringToFront();
            page.RefreshSpreadLayout();
            page.Focus();
        }
        pageHost.ResumeLayout(true);

        previousButton.Enabled = currentPage > 0;
        contentsButton.Enabled = currentPage != 0;
        nextButton.Enabled = currentPage < PageTitles.Length - 1;
        pageNumber.Text = $"PAGE {currentPage + 1} OF {PageTitles.Length}";
    }

    private void BuildManualPages()
    {
        pageHost.SuspendLayout();
        for (var pageIndex = 0; pageIndex < PageTitles.Length; pageIndex++)
        {
            var spread = new ManualSpread { Visible = false };
            pageHost.Controls.Add(spread);
            using var stagingPage = MakeStagingPanel();
            stagingPage.SuspendLayout();
            if (pageIndex == 0) BuildContents(stagingPage);
            else BuildArticle(stagingPage, pageIndex);
            stagingPage.ResumeLayout(true);
            spread.SetContent(stagingPage);
            switch (pageIndex)
            {
                case 2:
                    spread.SetWideArtwork(LoadEmbeddedImage("Kharvox.Branding.ControllerMapping"));
                    break;
                case 0:
                    spread.SetArtwork(LoadEmbeddedImage("Kharvox.Branding.ManualMark"), 0.56F, 0.38F);
                    break;
                case 8:
                    spread.SetArtwork(LoadEmbeddedImage("Kharvox.Branding.ManualMark"), 0.56F, 0.38F, true);
                    break;
                case 6:
                    spread.SetArtwork(LoadEmbeddedImage("Kharvox.Branding.BhapticsLogo"), 0.76F, 0.50F);
                    break;
                case 7:
                    spread.SetArtwork(LoadEmbeddedImage("Kharvox.Branding.Psvr2ToolkitLogo"), 0.58F, 0.50F);
                    break;
            }
            pages[pageIndex] = spread;
        }
        pageHost.ResumeLayout(false);
    }

    private static Image? LoadEmbeddedImage(string resourceName)
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(resourceName);
        if (stream is null) return null;
        using var embeddedImage = Image.FromStream(stream);
        return new Bitmap(embeddedImage);
    }

    private static FlowLayoutPanel MakeStagingPanel()
    {
        return new BufferedFlowLayoutPanel
        {
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            Margin = Padding.Empty
        };
    }

    private sealed class BufferedFlowLayoutPanel : FlowLayoutPanel
    {
        internal BufferedFlowLayoutPanel()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint, true);
            UpdateStyles();
        }
    }

    private sealed class ManualSpread : Panel
    {
        private readonly Panel leftViewport = MakeViewport();
        private readonly Panel rightViewport = MakeViewport();
        private readonly BufferedFlowLayoutPanel leftColumn = MakeColumn();
        private readonly BufferedFlowLayoutPanel rightColumn = MakeColumn();
        private readonly PictureBox rightArtwork = new()
        {
            BackColor = Color.Transparent,
            Enabled = false,
            SizeMode = PictureBoxSizeMode.Zoom,
            TabStop = false,
            Visible = false
        };
        private readonly PictureBox leftWideArtwork = new()
        {
            BackColor = Color.Transparent,
            SizeMode = PictureBoxSizeMode.Zoom,
            TabStop = false,
            Visible = false,
            AccessibleRole = AccessibleRole.Graphic,
            AccessibleName = "Left Quest controller input mapping diagram"
        };
        private readonly PictureBox rightWideArtwork = new()
        {
            BackColor = Color.Transparent,
            SizeMode = PictureBoxSizeMode.Zoom,
            TabStop = false,
            Visible = false,
            AccessibleRole = AccessibleRole.Graphic,
            AccessibleName = "Right Quest controller input mapping diagram"
        };
        private readonly List<Control> orderedControls = [];
        private readonly VScrollBar scrollBar = new()
        {
            SmallChange = 48,
            TabStop = true,
            Visible = false
        };
        private int preferredBreakIndex = -1;
        private bool forcePreferredBreak;
        private Control? wideArtworkPlaceholder;
        private Control? wideArtworkRightSpacer;

        internal ManualSpread()
        {
            Dock = DockStyle.Fill;
            Margin = Padding.Empty;
            BackColor = Color.Transparent;
            TabStop = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.SupportsTransparentBackColor, true);
            leftViewport.Controls.Add(leftColumn);
            rightViewport.Controls.Add(rightColumn);
            rightViewport.Controls.Add(rightArtwork);
            leftViewport.Controls.Add(leftWideArtwork);
            rightViewport.Controls.Add(rightWideArtwork);
            Controls.Add(leftViewport);
            Controls.Add(rightViewport);
            Controls.Add(scrollBar);
            scrollBar.ValueChanged += (_, _) => ApplyScrollPosition();
            MouseEnter += (_, _) => Focus();
            leftViewport.MouseEnter += (_, _) => Focus();
            rightViewport.MouseEnter += (_, _) => Focus();
            leftWideArtwork.MouseEnter += (_, _) => Focus();
            leftWideArtwork.MouseWheel += ScrollWithMouseWheel;
            rightWideArtwork.MouseEnter += (_, _) => Focus();
            rightWideArtwork.MouseWheel += ScrollWithMouseWheel;
            MouseWheel += ScrollWithMouseWheel;
            Resize += (_, _) => RefreshSpreadLayout();
        }

        internal void SetContent(FlowLayoutPanel stagingPage)
        {
            foreach (var control in stagingPage.Controls.Cast<Control>().ToArray())
            {
                stagingPage.Controls.Remove(control);
                if (Equals(control.Tag, ColumnBreakTag))
                {
                    if (preferredBreakIndex < 0)
                        preferredBreakIndex = orderedControls.Count;
                    control.Dispose();
                    continue;
                }
                if (Equals(control.Tag, ForcedColumnBreakTag))
                {
                    if (preferredBreakIndex < 0)
                        preferredBreakIndex = orderedControls.Count;
                    forcePreferredBreak = true;
                    control.Dispose();
                    continue;
                }
                if (Equals(control.Tag, WideArtworkPlaceholderTag))
                    wideArtworkPlaceholder = control;
                else if (Equals(control.Tag, WideArtworkRightSpacerTag))
                    wideArtworkRightSpacer = control;
                orderedControls.Add(control);
                HookMouseWheel(control);
            }
            RefreshSpreadLayout();
        }

        private float artworkWidthFraction = 0.65F;
        private float artworkHeightFraction = 0.50F;
        private bool artworkAlignBottom;

        internal void SetArtwork(Image? artwork, float widthFraction, float heightFraction,
            bool alignBottom = false)
        {
            rightArtwork.Image?.Dispose();
            rightArtwork.Image = artwork;
            artworkWidthFraction = Math.Max(0.20F, Math.Min(0.90F, widthFraction));
            artworkHeightFraction = Math.Max(0.20F, Math.Min(0.80F, heightFraction));
            artworkAlignBottom = alignBottom;
            rightArtwork.Visible = artwork is not null;
            RefreshSpreadLayout();
        }

        internal void SetWideArtwork(Image? artwork)
        {
            leftWideArtwork.Image?.Dispose();
            rightWideArtwork.Image?.Dispose();
            leftWideArtwork.Image = null;
            rightWideArtwork.Image = null;
            if (artwork is not null)
            {
                var halfWidth = artwork.Width / 2;
                leftWideArtwork.Image = CopyArtworkSlice(artwork,
                    new Rectangle(0, 0, halfWidth, artwork.Height));
                rightWideArtwork.Image = CopyArtworkSlice(artwork,
                    new Rectangle(artwork.Width - halfWidth, 0, halfWidth, artwork.Height));
                artwork.Dispose();
            }
            leftWideArtwork.Visible = leftWideArtwork.Image is not null;
            rightWideArtwork.Visible = rightWideArtwork.Image is not null;
            RefreshSpreadLayout();
        }

        private static Bitmap CopyArtworkSlice(Image artwork, Rectangle source)
        {
            var slice = new Bitmap(source.Width, source.Height,
                System.Drawing.Imaging.PixelFormat.Format32bppPArgb);
            using var graphics = Graphics.FromImage(slice);
            graphics.Clear(Color.Transparent);
            graphics.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighQuality;
            graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
            graphics.DrawImage(artwork, new Rectangle(Point.Empty, slice.Size), source,
                GraphicsUnit.Pixel);
            return slice;
        }

        internal void RefreshSpreadLayout()
        {
            if (ClientSize.Width <= 0 || ClientSize.Height <= 0) return;

            var sideMargin = Math.Max(48, (int)Math.Round(ClientSize.Width * 0.08));
            var topMargin = Math.Max(34, (int)Math.Round(ClientSize.Height * 0.08));
            var bottomMargin = Math.Max(38, (int)Math.Round(ClientSize.Height * 0.09));
            var gutter = Math.Max(28, (int)Math.Round(ClientSize.Width * 0.05));
            var pageWidth = Math.Max(160,
                (ClientSize.Width - (sideMargin * 2) - gutter) / 2);
            var pageHeight = Math.Max(160, ClientSize.Height - topMargin - bottomMargin);

            leftViewport.Bounds = new Rectangle(sideMargin, topMargin, pageWidth, pageHeight);
            rightViewport.Bounds = new Rectangle(
                sideMargin + pageWidth + gutter, topMargin, pageWidth, pageHeight);
            scrollBar.Bounds = new Rectangle(
                Math.Min(ClientSize.Width - scrollBar.Width - 4, rightViewport.Right + 5),
                topMargin,
                scrollBar.Width,
                pageHeight);

            if (leftWideArtwork.Image is { } artwork && wideArtworkPlaceholder is not null)
            {
                var naturalHeight = (int)Math.Round(
                    leftViewport.ClientSize.Width * artwork.Height / (double)artwork.Width);
                wideArtworkPlaceholder.Height = Math.Max(180,
                    Math.Min((int)Math.Round(pageHeight * 0.68), naturalHeight));
            }

            var split = forcePreferredBreak && preferredBreakIndex > 0;
            ArrangeColumns(split);
            LayoutColumn(leftColumn, leftViewport);
            if (split) AlignRightColumnBelowWideArtwork();
            LayoutColumn(rightColumn, rightViewport);
            if (!split && leftColumn.Height > leftViewport.ClientSize.Height && preferredBreakIndex > 0)
            {
                ArrangeColumns(true);
                LayoutColumn(leftColumn, leftViewport);
                AlignRightColumnBelowWideArtwork();
                LayoutColumn(rightColumn, rightViewport);
            }
            var overflow = Math.Max(
                Math.Max(0, leftColumn.Height - leftViewport.ClientSize.Height),
                Math.Max(0, rightColumn.Height - rightViewport.ClientSize.Height));
            var oldValue = scrollBar.Value;
            scrollBar.LargeChange = Math.Max(1, pageHeight);
            scrollBar.Maximum = Math.Max(0, overflow + scrollBar.LargeChange - 1);
            scrollBar.Visible = overflow > 0;
            scrollBar.Value = Math.Min(oldValue, overflow);
            ApplyScrollPosition();
            LayoutArtwork();
            leftViewport.Invalidate(true);
            rightViewport.Invalidate(true);
            Invalidate(true);
        }

        private void AlignRightColumnBelowWideArtwork()
        {
            if (wideArtworkPlaceholder?.Parent != leftColumn ||
                wideArtworkRightSpacer?.Parent != rightColumn)
                return;

            wideArtworkRightSpacer.Height = Math.Max(1,
                wideArtworkPlaceholder.Bottom + wideArtworkPlaceholder.Margin.Bottom -
                rightColumn.Padding.Top);
        }

        private void LayoutArtwork()
        {
            if (rightArtwork.Image is not { } artwork || rightViewport.ClientSize.Width <= 0 ||
                rightViewport.ClientSize.Height <= 0)
                return;

            var maximumWidth = Math.Max(1,
                (int)Math.Round(rightViewport.ClientSize.Width * artworkWidthFraction));
            var maximumHeight = Math.Max(1,
                (int)Math.Round(rightViewport.ClientSize.Height * artworkHeightFraction));
            var scale = Math.Min(maximumWidth / (double)artwork.Width,
                maximumHeight / (double)artwork.Height);
            var artworkSize = new Size(
                Math.Max(1, (int)Math.Round(artwork.Width * scale)),
                Math.Max(1, (int)Math.Round(artwork.Height * scale)));
            var artworkY = artworkAlignBottom
                ? Math.Max(0, rightViewport.ClientSize.Height - artworkSize.Height -
                    Math.Max(10, (int)Math.Round(rightViewport.ClientSize.Height * 0.05)))
                : (rightViewport.ClientSize.Height - artworkSize.Height) / 2;
            rightArtwork.Bounds = new Rectangle(
                (rightViewport.ClientSize.Width - artworkSize.Width) / 2,
                artworkY,
                artworkSize.Width,
                artworkSize.Height);
            rightArtwork.Visible = true;
            rightArtwork.BringToFront();
        }

        private void ArrangeColumns(bool split)
        {
            leftColumn.SuspendLayout();
            rightColumn.SuspendLayout();
            for (var index = 0; index < orderedControls.Count; index++)
            {
                var control = orderedControls[index];
                var target = split && index >= preferredBreakIndex ? rightColumn : leftColumn;
                if (!ReferenceEquals(control.Parent, target))
                {
                    control.Parent?.Controls.Remove(control);
                    target.Controls.Add(control);
                }
            }
            leftColumn.ResumeLayout(false);
            rightColumn.ResumeLayout(false);
        }

        private static Panel MakeViewport() => new()
        {
            BackColor = BookTextSurfaceColor,
            Margin = Padding.Empty
        };

        private static BufferedFlowLayoutPanel MakeColumn() => new()
        {
            AutoScroll = false,
            BackColor = BookTextSurfaceColor,
            FlowDirection = FlowDirection.TopDown,
            WrapContents = false,
            Padding = new Padding(9, 5, 9, 14),
            Margin = Padding.Empty,
            Location = Point.Empty
        };

        private static void LayoutColumn(FlowLayoutPanel column, Panel viewport)
        {
            column.SuspendLayout();
            column.Width = viewport.ClientSize.Width;
            var contentWidth = Math.Max(120, column.Width - column.Padding.Horizontal - 4);
            foreach (Control control in column.Controls)
            {
                control.Width = contentWidth;
                control.MaximumSize = new Size(contentWidth, 0);
            }
            column.ResumeLayout(true);
            column.PerformLayout();
            var contentBottom = column.Padding.Top;
            foreach (Control control in column.Controls)
                contentBottom = Math.Max(contentBottom, control.Bottom + control.Margin.Bottom);
            column.Height = Math.Max(viewport.ClientSize.Height,
                contentBottom + column.Padding.Bottom);
        }

        private void ApplyScrollPosition()
        {
            leftColumn.Location = new Point(0, -scrollBar.Value);
            rightColumn.Location = new Point(0, -scrollBar.Value);
            LayoutWideArtwork();
        }

        private void LayoutWideArtwork()
        {
            if (leftWideArtwork.Image is null || rightWideArtwork.Image is null ||
                wideArtworkPlaceholder is null ||
                leftViewport.ClientSize.Width <= 0 || rightViewport.ClientSize.Width <= 0)
            {
                leftWideArtwork.Visible = false;
                rightWideArtwork.Visible = false;
                return;
            }

            var artworkHeight = wideArtworkPlaceholder.Height;
            leftWideArtwork.Bounds = new Rectangle(
                0,
                wideArtworkPlaceholder.Top - scrollBar.Value,
                leftViewport.ClientSize.Width,
                artworkHeight);
            rightWideArtwork.Bounds = new Rectangle(
                0,
                wideArtworkPlaceholder.Top - scrollBar.Value,
                rightViewport.ClientSize.Width,
                artworkHeight);
            leftWideArtwork.Visible = true;
            rightWideArtwork.Visible = true;
            leftWideArtwork.BringToFront();
            rightWideArtwork.BringToFront();
            scrollBar.BringToFront();
        }

        private void ScrollWithMouseWheel(object? sender, MouseEventArgs e)
        {
            if (!scrollBar.Visible) return;
            var step = Math.Max(scrollBar.SmallChange, Font.Height * 3);
            var requested = scrollBar.Value + (e.Delta < 0 ? step : -step);
            var maximumValue = Math.Max(0, scrollBar.Maximum - scrollBar.LargeChange + 1);
            scrollBar.Value = Math.Max(0, Math.Min(maximumValue, requested));
        }

        private void HookMouseWheel(Control control)
        {
            control.MouseEnter += (_, _) => Focus();
            control.MouseWheel += ScrollWithMouseWheel;
            foreach (Control child in control.Controls)
                HookMouseWheel(child);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                rightArtwork.Image?.Dispose();
                rightArtwork.Image = null;
                leftWideArtwork.Image?.Dispose();
                leftWideArtwork.Image = null;
                rightWideArtwork.Image?.Dispose();
                rightWideArtwork.Image = null;
            }
            base.Dispose(disposing);
        }
    }

    private sealed class BookPageHost : Panel
    {
        private Image? bookImage;
        private Bitmap? scaledBookImage;

        internal BookPageHost()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint, true);
            ResizeRedraw = true;
            UpdateStyles();
        }

        internal Image? BookImage
        {
            get => bookImage;
            set
            {
                bookImage?.Dispose();
                bookImage = value;
                RebuildScaledBookImage();
                Invalidate();
            }
        }

        protected override void OnSizeChanged(EventArgs e)
        {
            base.OnSizeChanged(e);
            RebuildScaledBookImage();
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            e.Graphics.Clear(BackColor);
            if (scaledBookImage is not null)
                e.Graphics.DrawImageUnscaled(scaledBookImage, Point.Empty);
        }

        private void RebuildScaledBookImage()
        {
            scaledBookImage?.Dispose();
            scaledBookImage = null;
            if (bookImage is null || ClientSize.Width <= 0 || ClientSize.Height <= 0) return;

            var scaled = new Bitmap(ClientSize.Width, ClientSize.Height,
                System.Drawing.Imaging.PixelFormat.Format32bppPArgb);
            using (var graphics = Graphics.FromImage(scaled))
            {
                graphics.Clear(BackColor);
                graphics.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceOver;
                graphics.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighQuality;
                graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                graphics.DrawImage(bookImage, new Rectangle(Point.Empty, ClientSize));
            }
            scaledBookImage = scaled;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                scaledBookImage?.Dispose();
                scaledBookImage = null;
                bookImage?.Dispose();
                bookImage = null;
            }
            base.Dispose(disposing);
        }
    }

    private void BuildContents(FlowLayoutPanel page)
    {
        AddChapterMarker(page, "START HERE");
        AddTitle(page, "Contents");
        AddParagraph(page,
            "Select a chapter to open it. Read in order with Previous and Next, or return here at any time with Contents.");
        AddDivider(page);

        for (var index = 1; index < PageTitles.Length; index++)
        {
            var target = index;
            var link = new LinkLabel
            {
                Text = $"{index:00}   {PageTitles[index]}",
                AutoSize = true,
                LinkColor = Color.Gainsboro,
                ActiveLinkColor = AccentColor,
                VisitedLinkColor = Color.Gainsboro,
                Font = new Font("Segoe UI Semibold", 11F, FontStyle.Bold),
                LinkBehavior = LinkBehavior.HoverUnderline,
                TextAlign = ContentAlignment.MiddleLeft,
                Cursor = Cursors.Hand,
                Padding = new Padding(0, 7, 0, 7),
                Margin = new Padding(0, 0, 0, 2),
                TabStop = true
            };
            link.LinkClicked += (_, _) => ShowPage(target);
            page.Controls.Add(link);
        }
    }

    private void BuildArticle(FlowLayoutPanel page, int pageIndex)
    {
        AddChapterMarker(page, $"CHAPTER {pageIndex:00}");
        AddTitle(page, PageTitles[pageIndex]);
        AddDivider(page);

        switch (pageIndex)
        {
            case 1: BuildGeneralInformation(page); break;
            case 2: BuildControllerInput(page); break;
            case 3: BuildMovement(page); break;
            case 4: BuildGameOptions(page); break;
            case 5: BuildRendering(page); break;
            case 6: BuildBhaptics(page); break;
            case 7: BuildPsvr2Toolkit(page); break;
            case 8: BuildAbout(page); break;
        }
    }

    private static void BuildGeneralInformation(FlowLayoutPanel page)
    {
        AddParagraph(page,
            "KHARVOX is a total VR conversion for DOOM (2016). It adds room-scale 6DoF movement and tracked motion-controller input to the original campaign.");
        AddHeading(page, "Requirements");
        AddParagraph(page,
            "A legally acquired, complete PC version of DOOM (2016) is required. KHARVOX has been tested with the Steam release and does not include the game or any game assets.");
        AddHeading(page, "Best practice");
        AddBullet(page, "Use SFS Renderer.");
        AddBullet(page, "Quest headsets should use Virtual Desktop or Meta Quest Link.");
        AddBullet(page, "Keep the DOOM game window in focus while playing.");
        AddColumnBreak(page);
        AddHeading(page, "Limitations");
        AddParagraph(page,
            "KHARVOX changes the game's rendering, camera and controls, so compatibility or startup issues may occur. Some hardware, including AMD graphics cards and less common headsets, may need additional setup. If a launch fails, close any remaining DOOM window and try Launch Game again.");
        AddHeading(page, "Community support");
        AddParagraph(page,
            "For general setup help, testing feedback and community support, visit the Flat2VR Discord server.");
        AddExternalLink(page, "Open the Flat2VR Discord", Flat2VrDiscordUrl);
    }

    private static void BuildControllerInput(FlowLayoutPanel page)
    {
        AddWideArtworkPlaceholder(page);
        AddHeading(page, "Left controller");
        AddCodeBlock(page,
            "INPUT             ACTION\n" +
            "Trigger           Equipment / grenade\n" +
            "Grip near weapon  Two-hand support\n" +
            "Grip tap away     Next equipment\n" +
            "Grip hold         BFG (no support grip)\n" +
            "X                 Dossier\n" +
            "Y                 Switch weapon mod\n" +
            "Stick             Move\n" +
            "Stick click       Menu / Pause");
        AddHeading(page, "Menus");
        AddParagraph(page,
            "A confirms and B goes back. Full-screen interfaces preserve the selected handedness layout. Physical Glory Kills follow the launcher setting rather than replacing normal menu input.");
        AddColumnBreak(page);
        AddWideArtworkRightSpacer(page);
        AddHeading(page, "Right controller");
        AddCodeBlock(page,
            "INPUT               ACTION\n" +
            "Trigger             Fire\n" +
            "Grip (front)        Weapon mod / alt fire\n" +
            "Grip (shoulder)     Shoulder Weapon\n" +
            "A                   Crouch\n" +
            "B                   Jump\n" +
            "Stick left / right  Turn\n" +
            "Stick up            Chainsaw\n" +
            "Stick down (tap)    Switch weapon\n" +
            "Stick down (hold)   Weapon wheel\n" +
            "                    Aim with left stick\n" +
            "Stick click         Use / Melee / Glory Kill");
        AddHeading(page, "Motion weapon wheel");
        AddBullet(page, "Hold the weapon-selection stick down as usual, then move the right weapon hand sideways or vertically to select a slot. Release the opening stick to confirm. In Left Hand mode, move the left weapon hand instead.");
        AddBullet(page, "The selection stick takes priority only while deflected beyond its 0.3 radial deadzone. Center it to resume hand selection immediately. Sector clicks vibrate the controller used for selection: left for stick, right for motion with the default mapping. Tracking loss clears the motion origin; recovery captures a new hand position.");
        AddHeading(page, "Left Hand mode — Button swap");
        AddBullet(page, "Left Trigger / Grip become Fire and weapon mod; the left hand holds the weapon.");
        AddBullet(page, "Right Trigger / Grip become Equipment and two-hand support.");
        AddBullet(page, "Left Stick click becomes Use, Melee / Glory Kill; Right Stick click becomes Menu / Pause.");
        AddBullet(page, "Face buttons and stick axes stay in their standard physical positions.");
        AddHeading(page, "Left Hand mode — Button and Stick swap");
        AddBullet(page, "Uses the trigger, grip and stick-click swap described above.");
        AddBullet(page, "Right Stick moves; Left Stick turns and controls weapon selection.");
        AddBullet(page, "Left Y jumps and Left X crouches.");
        AddBullet(page, "Right A opens the Dossier and Right B switches the weapon modification.");
    }

    private static void AddWideArtworkPlaceholder(FlowLayoutPanel page)
    {
        page.Controls.Add(new Panel
        {
            Height = 300,
            BackColor = BookTextSurfaceColor,
            Margin = new Padding(0, 2, 0, 12),
            Tag = WideArtworkPlaceholderTag
        });
    }

    private static void AddWideArtworkRightSpacer(FlowLayoutPanel page)
    {
        page.Controls.Add(new Panel
        {
            Height = 300,
            BackColor = BookTextSurfaceColor,
            Margin = Padding.Empty,
            Tag = WideArtworkRightSpacerTag
        });
    }

    private static void BuildMovement(FlowLayoutPanel page)
    {
        AddParagraph(page,
            "Locomotion is head-relative: pushing the movement stick forward moves in the direction you are looking. Physical room-scale movement is translated into the game separately.");
        AddHeading(page, "Turn mode");
        AddBullet(page, "Smooth — continuous turning. The Slow-to-Fast slider ranges from 150°/s to 400°/s in 1°/s steps; the default is 230°/s.");
        AddBullet(page, "Snap — rotates by the configured snap angle each time the turn stick is pushed sideways.");
        AddBullet(page, "Off — disables artificial stick turning. Physical turning and room-scale movement remain available.");
        AddHeading(page, "Movement direction");
        AddBullet(page, "Head direction — moving forward follows the horizontal headset direction. This is the default and existing KHARVOX behaviour.");
        AddBullet(page, "Off hand direction — moving forward follows the horizontal aim direction of the non-weapon controller. Right-handed mode uses the left controller; Left Hand mode uses the right controller. Invalid tracking or a nearly vertical controller safely falls back to Head direction.");
        AddColumnBreak(page);
        AddHeading(page, "Left Hand mode");
        AddParagraph(page,
            "Left Hand mode moves the tracked weapon to the left hand and the support role to the right. Button swap keeps movement on the left stick and turning on the right. Button and Stick swap moves locomotion to the right stick and turning / weapon selection to the left stick, while also moving the face-button actions to the opposite controllers.");
    }

    private static void BuildGameOptions(FlowLayoutPanel page)
    {
        AddHeading(page, "Immersive Mode");
        AddParagraph(page,
            "Keeps supported Glory Kills, traversal sequences, tutorials and cinematics in stereoscopic VR instead of presenting the complete frame on a flat Cine Window. This is more immersive, but authored camera motion can be uncomfortable.");
        AddBullet(page, "Freelook in cinematics and Glory Kills — lets headset rotation control the view while the authored sequence continues.");
        AddBullet(page, "Regular cinematics in Cine Window — keeps interactive VR moments immersive, but presents regular scripted cinematics on the comfort window.");
        AddHeading(page, "Cine Window follows headset");
        AddParagraph(page,
            "Makes the virtual screen used for cinematics follow your headset. When disabled, the screen stays in a fixed position. Menus and other VR elements are unaffected.");
        AddColumnBreak(page);
        AddHeading(page, "Physical Glory Kills");
        AddParagraph(page,
            "A forward punch from the selected hand or hands can trigger Melee / Glory Kill. The Slow-to-Fast threshold slider ranges from 1.0 m/s to 4.0 m/s in 0.2 m/s steps; the default is 2.8 m/s. Move it toward Fast if normal hand movement causes accidental attacks.");
        AddHeading(page, "Shoulder Weapon");
        AddParagraph(page,
            "Selects the weapon assigned to the behind-the-shoulder Grip gesture. Move the weapon hand behind its matching shoulder and make a fresh Grip press. If the selected weapon is unavailable or empty, KHARVOX tries the Combat Shotgun and then the Pistol. It never unlocks or grants a weapon.");
        AddHeading(page, "Virtual Gunstock");
        AddParagraph(page,
            "Adds a headset-relative rear anchor to calibrated two-handed, barrel-aligned weapons after the support grip is acquired. This can make long-range aiming steadier while preserving tracked controller movement.");
        AddHeading(page, "Hands Jump");
        AddParagraph(page, "Raise both hands to jump. You can still use the jump button. Works with either renderer, even when hand models are hidden. Disabled in menus and cinematics.");
        AddHeading(page, "Laser sight");
        AddParagraph(page,
            "Projects an aiming line from the rendered muzzle of supported firearms. It is an optional aiming aid and has no effect on weapon accuracy or projectile behavior.");
    }

    private static void BuildRendering(FlowLayoutPanel page)
    {
        AddHeading(page, "Renderer selection");
        AddParagraph(page, "SFS is the default renderer. If SFS causes problems, select AER as a fallback. Resolution changes require restarting the game.");
        AddHeading(page, "RenderScale");
        AddParagraph(page,
            "Render scale adjusts the resolution used to render the game. Higher values can improve clarity but demand more GPU power; lower values can improve performance at the cost of detail. The maximum supported resolution depends on your graphics card and VR runtime. Restart the game after changing this setting.");
        AddHeading(page, "FSR Upscaling");
        AddBullet(page, "With Meta XR, VDXR or SteamVR, select SFS or AER, choose a RenderScale below 100%, and enable Use FSR Upscaling beside it.");
        AddBullet(page, "Start at 80%. Lower values reduce more source-pixel work but also lose fine detail.");
        AddBullet(page, "FSR remains inactive at 100% or higher. When enabled below 100%, verify FSR1 active in the launcher status line.");
        AddColumnBreak(page);
        AddHeading(page, "Recommended runtimes");
        AddBullet(page, "Quest / Pico — Virtual Desktop with VDXR.");
        AddBullet(page, "Valve Index / PSVR2 — SteamVR.");
        AddParagraph(page,
            "Use the highest stable headset refresh rate your system can sustain. Test with SFS first and include the headset, GPU and runtime when reporting a rendering issue.");
        AddHeading(page, "VR-safe image settings");
        AddParagraph(page,
            "Every KHARVOX launch disables DOOM's native Motion Blur, automatic Glory Kill Motion Blur and Chromatic Aberration. These VR-safe overrides do not edit or replace the encrypted DOOM user profile or campaign savegames.");
    }

    private static void BuildBhaptics(FlowLayoutPanel page)
    {
        AddParagraph(page,
            "bHaptics feedback requires compatible bHaptics gear and the bHaptics Player for Windows. Pair and verify the devices in the Player before starting the game.");
        AddColumnBreak(page);
        AddParagraph(page,
            "Enable bHaptics may remain selected when no bHaptics device is connected. Missing hardware, an unavailable Player or a local bridge error does not block DOOM from starting; there will simply be no suit feedback. Normal controller rumble remains independent.");
        AddExternalLink(page, "Download the official bHaptics Player", BhapticsPlayerUrl);
    }

    private static void BuildPsvr2Toolkit(FlowLayoutPanel page)
    {
        AddParagraph(page,
            "Use PSVR2 Toolkit is experimental and works only with PlayStation VR2 hardware. It requires PSVR2 Toolkit 1.0 or later and a working PSVR2 SteamVR setup. With other headsets, the option has no effect.");
        AddParagraph(page,
            "KHARVOX uses it for weapon-specific adaptive-trigger resistance on the weapon hand. The support hand remains disabled and normal controller rumble is unaffected. Missing or inactive Toolkit software never blocks game startup.");
        AddParagraph(page,
            "Install and update PSVR2 Toolkit separately.");
        AddExternalLink(page, "Open the PSVR2 Toolkit repository", Psvr2ToolkitUrl);
        AddExternalLink(page, "Open the PSVR2 Toolkit Discord support", Psvr2ToolkitSupportUrl);
    }

    private static void BuildAbout(FlowLayoutPanel page)
    {
        AddHeading(page, "KHARVOX — An independent VR compatibility framework");
        AddParagraph(page,
            "KHARVOX is an independent community project and is not affiliated with, endorsed by, or sponsored by the publishers or developers of supported games.");
        AddParagraph(page,
            "All trademarks and product names are the property of their respective owners. A legally acquired installation of each supported game is required.");
        var notices = new LinkLabel { Text = "Licenses and third-party notices", AutoSize = true,
            LinkColor = Color.LightSkyBlue, Margin = new Padding(0, 8, 0, 12) };
        notices.LinkClicked += (_, _) => {
            using var dialog = new Form { Text = "Licenses and third-party notices",
                Size = new Size(820, 640), MinimumSize = new Size(500, 350),
                StartPosition = FormStartPosition.CenterParent, ShowInTaskbar = false };
            dialog.Controls.Add(new TextBox { Multiline = true, ReadOnly = true,
                ScrollBars = ScrollBars.Both, Dock = DockStyle.Fill, WordWrap = false,
                Font = new Font("Consolas", 10F), Text = LicenseNotices() });
            dialog.ShowDialog(page.FindForm());
        };
        page.Controls.Add(notices);
        AddForcedColumnBreak(page);
        AddParagraph(page, "Made by Cactus", AccentColor, true);
        AddHeading(page, "Contributors");
        AddExternalLink(page, "Cabalistic", "https://github.com/fholger");
        AddExternalLink(page, "Dilshan", "https://dilshanvisuals.com");
        AddParagraph(page, "TinyBlackDog");
        AddHeading(page, "Thanks");
        AddParagraph(page,
            "Crementif, Hoshi82, Galaghan, thefreemike, nabelo, VR DaD, Tino, Vince Crusty, BaggyG and Team Beef.");
    }

    internal static string LicenseNotices()
    {
        var assembly = Assembly.GetExecutingAssembly();
        var texts = new List<string>();
        foreach (var name in assembly.GetManifestResourceNames()
            .Where(name => name.StartsWith("Kharvox.Licenses.", StringComparison.Ordinal)).OrderBy(name => name))
        {
            using var stream = assembly.GetManifestResourceStream(name)
                ?? throw new InvalidOperationException("Missing notice: " + name);
            using var reader = new StreamReader(stream);
            texts.Add(name.Substring("Kharvox.Licenses.".Length) + "\r\n\r\n" +
                reader.ReadToEnd().Replace("\r\n", "\n").Replace("\n", "\r\n"));
        }
        return string.Join("\r\n\r\n----------------------------------------\r\n\r\n", texts);
    }

    private static Button MakeNavigationButton(string text)
    {
        var button = new Button
        {
            Text = text,
            Dock = DockStyle.Fill,
            FlatStyle = FlatStyle.Flat,
            BackColor = PanelColor,
            ForeColor = Color.WhiteSmoke,
            Font = new Font("Segoe UI Semibold", 9F, FontStyle.Bold),
            Margin = new Padding(0, 0, 8, 0),
            Cursor = Cursors.Hand
        };
        button.FlatAppearance.BorderColor = Color.DimGray;
        button.FlatAppearance.MouseOverBackColor = Color.FromArgb(48, 48, 52);
        return button;
    }

    private static void AddChapterMarker(FlowLayoutPanel page, string text) =>
        AddText(page, text, new Font("Segoe UI Semibold", 8.5F, FontStyle.Bold),
            AccentColor, new Padding(0, 0, 0, 3));

    private static void AddTitle(FlowLayoutPanel page, string text) =>
        AddText(page, text, new Font("Segoe UI Semibold", 21F, FontStyle.Bold),
            Color.WhiteSmoke, new Padding(0, 0, 0, 12));

    private static void AddHeading(FlowLayoutPanel page, string text) =>
        AddText(page, text, new Font("Segoe UI Semibold", 12F, FontStyle.Bold),
            Color.WhiteSmoke, new Padding(0, 11, 0, 4));

    private static void AddParagraph(FlowLayoutPanel page, string text) =>
        AddParagraph(page, text, Color.Gainsboro, false);

    private static void AddParagraph(FlowLayoutPanel page, string text, Color color, bool bold) =>
        AddText(page, text,
            new Font("Segoe UI", 10F, bold ? FontStyle.Bold : FontStyle.Regular),
            color, new Padding(0, 0, 0, 10));

    private static void AddBullet(FlowLayoutPanel page, string text) =>
        AddText(page, "•  " + text, new Font("Segoe UI", 10F), Color.Gainsboro,
            new Padding(12, 0, 0, 7));

    private static void AddCodeBlock(FlowLayoutPanel page, string text)
    {
        var label = new Label
        {
            Text = text,
            AutoSize = true,
            Font = new Font("Consolas", 8.5F),
            ForeColor = Color.Gainsboro,
            BackColor = Color.FromArgb(15, 15, 17),
            BorderStyle = BorderStyle.FixedSingle,
            Padding = new Padding(10, 11, 10, 11),
            Margin = new Padding(0, 2, 0, 10),
            UseMnemonic = false
        };
        page.Controls.Add(label);
    }

    private static void AddDivider(FlowLayoutPanel page)
    {
        page.Controls.Add(new Panel
        {
            Height = 1,
            BackColor = Color.FromArgb(70, 70, 74),
            Margin = new Padding(0, 0, 0, 17)
        });
    }

    private static void AddColumnBreak(FlowLayoutPanel page)
    {
        page.Controls.Add(new Panel
        {
            Size = Size.Empty,
            Tag = ColumnBreakTag,
            Visible = false
        });
    }

    private static void AddForcedColumnBreak(FlowLayoutPanel page)
    {
        page.Controls.Add(new Panel
        {
            Size = Size.Empty,
            Tag = ForcedColumnBreakTag,
            Visible = false
        });
    }

    private static void AddText(FlowLayoutPanel page, string text, Font font,
        Color color, Padding margin)
    {
        page.Controls.Add(new Label
        {
            Text = text,
            AutoSize = true,
            Font = font,
            ForeColor = color,
            BackColor = Color.Transparent,
            Margin = margin,
            UseMnemonic = false
        });
    }

    private static void AddExternalLink(FlowLayoutPanel page, string text, string url)
    {
        var link = new LinkLabel
        {
            Text = text + "  ↗",
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 10F, FontStyle.Bold),
            LinkColor = AccentColor,
            ActiveLinkColor = Color.FromArgb(255, 120, 100),
            VisitedLinkColor = AccentColor,
            LinkBehavior = LinkBehavior.HoverUnderline,
            Cursor = Cursors.Hand,
            Margin = new Padding(0, 1, 0, 10),
            UseMnemonic = false
        };
        link.LinkClicked += (_, _) => OpenExternalLink(url);
        page.Controls.Add(link);
    }

    private static void OpenExternalLink(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttps && uri.Scheme != Uri.UriSchemeHttp))
            return;
        try
        {
            Process.Start(new ProcessStartInfo(uri.AbsoluteUri) { UseShellExecute = true });
        }
        catch
        {
            MessageBox.Show(
                "The link could not be opened.\n\n" + uri.AbsoluteUri,
                "KHARVOX Instructions",
                MessageBoxButtons.OK,
                MessageBoxIcon.Information);
        }
    }
}
