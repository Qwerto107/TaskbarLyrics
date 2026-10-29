// WPF 负责字形排版和字体回退，Skia 负责抗锯齿与阴影，结果按实际 DPI 缓存在位图中。
using System.Globalization;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using SkiaSharp;

namespace TaskbarLyrics;

/// <summary>可选 Skia 或原生 TextBlock 的文字层，共用外层布局、裁剪和动画。</summary>
public sealed class SkiaTextBlock : FrameworkElement
{
    private const FrameworkPropertyMetadataOptions TextFlags =
        FrameworkPropertyMetadataOptions.AffectsMeasure | FrameworkPropertyMetadataOptions.AffectsRender;

    public static readonly DependencyProperty TextProperty = TextBlock.TextProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata("", TextFlags, TextChanged));
    public static readonly DependencyProperty FontFamilyProperty = TextElement.FontFamilyProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata(new FontFamily("Microsoft YaHei UI"), TextFlags, TextChanged));
    public static readonly DependencyProperty FontSizeProperty = TextElement.FontSizeProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata(16.0, TextFlags, TextChanged));
    public static readonly DependencyProperty FontWeightProperty = TextElement.FontWeightProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata(FontWeights.Normal, TextFlags, TextChanged));
    public static readonly DependencyProperty ForegroundProperty = TextElement.ForegroundProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata(Brushes.White, TextFlags, TextChanged));
    public static readonly DependencyProperty LineHeightProperty = TextBlock.LineHeightProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata(double.NaN, TextFlags, TextChanged));
    public static readonly DependencyProperty TextTrimmingProperty = TextBlock.TextTrimmingProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata(TextTrimming.None, TextFlags, TextChanged));
    public static readonly DependencyProperty TextWrappingProperty = TextBlock.TextWrappingProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata(TextWrapping.NoWrap, TextFlags, TextChanged));
    public static readonly DependencyProperty TextAlignmentProperty = TextBlock.TextAlignmentProperty.AddOwner(
        typeof(SkiaTextBlock), new FrameworkPropertyMetadata(TextAlignment.Left, TextFlags, TextChanged));
    public static readonly DependencyProperty HintingProperty = DependencyProperty.RegisterAttached(
        "Hinting", typeof(SKFontHinting), typeof(SkiaTextBlock),
        new FrameworkPropertyMetadata(SKFontHinting.Normal,
            FrameworkPropertyMetadataOptions.Inherits | FrameworkPropertyMetadataOptions.AffectsRender, TextChanged));
    public static readonly DependencyProperty RendererProperty = DependencyProperty.RegisterAttached(
        "Renderer", typeof(string), typeof(SkiaTextBlock),
        new FrameworkPropertyMetadata("skia", TextFlags | FrameworkPropertyMetadataOptions.Inherits, TextChanged));
    public static readonly DependencyProperty TextShadowProperty = DependencyProperty.Register(
        nameof(TextShadow), typeof(DropShadowEffect), typeof(SkiaTextBlock),
        new FrameworkPropertyMetadata(null, TextFlags, TextChanged));

    public string Text { get => (string)GetValue(TextProperty); set => SetValue(TextProperty, value); }
    public FontFamily FontFamily { get => (FontFamily)GetValue(FontFamilyProperty); set => SetValue(FontFamilyProperty, value); }
    public double FontSize { get => (double)GetValue(FontSizeProperty); set => SetValue(FontSizeProperty, value); }
    public FontWeight FontWeight { get => (FontWeight)GetValue(FontWeightProperty); set => SetValue(FontWeightProperty, value); }
    public Brush Foreground { get => (Brush)GetValue(ForegroundProperty); set => SetValue(ForegroundProperty, value); }
    public double LineHeight { get => (double)GetValue(LineHeightProperty); set => SetValue(LineHeightProperty, value); }
    public TextTrimming TextTrimming { get => (TextTrimming)GetValue(TextTrimmingProperty); set => SetValue(TextTrimmingProperty, value); }
    public TextWrapping TextWrapping { get => (TextWrapping)GetValue(TextWrappingProperty); set => SetValue(TextWrappingProperty, value); }
    public TextAlignment TextAlignment { get => (TextAlignment)GetValue(TextAlignmentProperty); set => SetValue(TextAlignmentProperty, value); }
    public DropShadowEffect? TextShadow { get => (DropShadowEffect?)GetValue(TextShadowProperty); set => SetValue(TextShadowProperty, value); }

    public static SKFontHinting GetHinting(DependencyObject target) => (SKFontHinting)target.GetValue(HintingProperty);
    public static void SetHinting(DependencyObject target, SKFontHinting value) => target.SetValue(HintingProperty, value);
    public static string GetRenderer(DependencyObject target) => (string)target.GetValue(RendererProperty);
    public static void SetRenderer(DependencyObject target, string value) => target.SetValue(RendererProperty, value == "wpf" ? "wpf" : "skia");

    internal static void ApplyHinting(DependencyObject target, string mode) => SetHinting(target, mode switch
    {
        "none" => SKFontHinting.None,
        "slight" => SKFontHinting.Slight,
        "full" => SKFontHinting.Full,
        _ => SKFontHinting.Normal,
    });

    private BitmapSource? _raster;
    private Rect _rasterRect;
    private Size _rasterSize;
    private TextBlock? _native;
    private bool UseWpf => GetRenderer(this) == "wpf";

    protected override int VisualChildrenCount => _native == null ? 0 : 1;
    protected override Visual GetVisualChild(int index) => index == 0 && _native != null
        ? _native : throw new ArgumentOutOfRangeException(nameof(index));

    static SkiaTextBlock()
    {
        TextOptions.TextFormattingModeProperty.OverrideMetadata(typeof(SkiaTextBlock),
            new FrameworkPropertyMetadata(TextFormattingMode.Ideal,
                TextFlags | FrameworkPropertyMetadataOptions.Inherits, TextChanged));
    }

    public SkiaTextBlock()
    {
        // 切行后的旧层不再显示，尽早释放位图；重新入树时由 OnRender 重建。
        Unloaded += (_, _) => { _raster = null; InvalidateVisual(); };
    }

    private static void TextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        // 引擎和 Hinting 同时挂在窗口和弹出层上，只有文字层持有需要失效的位图。
        if (d is SkiaTextBlock text) text._raster = null;
    }

    private FormattedText Format(double width = double.PositiveInfinity)
    {
        var text = new FormattedText(Text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface(FontFamily, FontStyles.Normal, FontWeight, FontStretches.Normal),
            FontSize, Foreground, null, TextOptions.GetTextFormattingMode(this), VisualTreeHelper.GetDpi(this).PixelsPerDip);
        text.LineHeight = double.IsNaN(LineHeight) ? Math.Ceiling(FontSize * 1.3) : LineHeight;
        text.TextAlignment = TextAlignment;
        if ((TextWrapping != TextWrapping.NoWrap || TextTrimming != TextTrimming.None) && double.IsFinite(width))
        {
            text.MaxTextWidth = Math.Max(0.01, width);
            text.Trimming = TextTrimming;
            if (TextWrapping == TextWrapping.NoWrap) text.MaxLineCount = 1;
        }
        return text;
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        if (UseWpf)
        {
            // 在布局阶段建子元素，避免继承属性传播过程中改动视觉树。
            if (_native == null)
            {
                _native = new TextBlock();
                AddVisualChild(_native);
            }
            _native.Text = Text;
            _native.FontFamily = FontFamily;
            _native.FontSize = FontSize;
            _native.FontWeight = FontWeight;
            _native.Foreground = Foreground;
            _native.TextWrapping = TextWrapping;
            _native.TextTrimming = TextTrimming;
            _native.TextAlignment = TextAlignment;
            _native.LineHeight = LineHeight;
            _native.LineStackingStrategy = double.IsNaN(LineHeight)
                ? LineStackingStrategy.MaxHeight : LineStackingStrategy.BlockLineHeight;
            _native.Effect = TextShadow;
            _native.Measure(availableSize);
            return _native.DesiredSize;
        }
        if (_native != null)
        {
            RemoveVisualChild(_native);
            _native = null;
        }
        var text = Format(availableSize.Width);
        var width = text.WidthIncludingTrailingWhitespace;
        if (TextWrapping != TextWrapping.NoWrap || TextTrimming != TextTrimming.None) width = Math.Min(width, availableSize.Width);
        return new Size(width, text.Height);
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        _native?.Arrange(new Rect(finalSize));
        return finalSize;
    }

    protected override void OnDpiChanged(DpiScale oldDpi, DpiScale newDpi)
    {
        base.OnDpiChanged(oldDpi, newDpi);
        _raster = null;
        InvalidateMeasure();
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        if (UseWpf) return; // 原生子 TextBlock 由 WPF 自己绘制，不经过 Skia 位图。
        if (Text.Length == 0 || RenderSize.Width <= 0 || RenderSize.Height <= 0) return;
        // 裁剪和透明度动画只重用位图，不逐帧重新排版或创建原生字体资源。
        if (_raster == null || _rasterSize != RenderSize) Rasterize();
        drawingContext.DrawImage(_raster, _rasterRect);
    }

    private void Rasterize()
    {
        var text = Format(RenderSize.Width);
        var visual = new DrawingVisual();
        using (var dc = visual.RenderOpen()) dc.DrawText(text, new Point());
        var dpi = VisualTreeHelper.GetDpi(this);
        var scale = dpi.PixelsPerDip;
        // 阴影和字形悬垂不计入排版宽度，但位图留足空间，避免切掉首尾笔画。
        var pad = Math.Ceiling(Math.Max(3, TextShadow?.BlurRadius ?? 0) + 2);
        var width = Math.Max(1, (int)Math.Ceiling((RenderSize.Width + pad * 2) * scale));
        var height = Math.Max(1, (int)Math.Ceiling((Math.Max(text.Height, RenderSize.Height) + pad * 2) * scale));
        using var bitmap = new SKBitmap(width, height, SKColorType.Bgra8888, SKAlphaType.Premul);
        using (var canvas = new SKCanvas(bitmap))
        {
            canvas.Clear(SKColors.Transparent);
            canvas.Scale((float)scale);
            canvas.Translate((float)pad, (float)pad);
            DrawGlyphs(canvas, visual.Drawing);
            canvas.Flush();
        }
        var raster = new WriteableBitmap(width, height, dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32, null);
        raster.WritePixels(new Int32Rect(0, 0, width, height), bitmap.GetPixels(), bitmap.ByteCount, bitmap.RowBytes);
        raster.Freeze();
        _raster = raster;
        _rasterSize = RenderSize;
        _rasterRect = new Rect(-pad, -pad, width / scale, height / scale);
    }

    private void DrawGlyphs(SKCanvas canvas, Drawing drawing)
    {
        if (drawing is DrawingGroup group)
        {
            foreach (var child in group.Children) DrawGlyphs(canvas, child);
            return;
        }
        if (drawing is not GlyphRunDrawing glyphDrawing) return;
        var run = glyphDrawing.GlyphRun;
        var typeface = run.GlyphTypeface;
        var family = typeface.FamilyNames.TryGetValue(CultureInfo.GetCultureInfo("en-US"), out var english)
            ? english : typeface.FamilyNames.Values.First();
        using var style = new SKFontStyle(typeface.Weight.ToOpenTypeWeight(), typeface.Stretch.ToOpenTypeStretch(),
            typeface.Style == FontStyles.Normal ? SKFontStyleSlant.Upright : SKFontStyleSlant.Italic);
        using var face = SKTypeface.FromFamilyName(family, style);
        using var font = new SKFont(face, (float)run.FontRenderingEmSize)
        {
            Edging = SKFontEdging.Antialias,
            Hinting = GetHinting(this),
        };
        font.Embolden = (typeface.StyleSimulations & StyleSimulations.BoldSimulation) != 0;
        if ((typeface.StyleSimulations & StyleSimulations.ItalicSimulation) != 0) font.SkewX = -0.2f;
        var glyphs = run.GlyphIndices.ToArray();
        var positions = new SKPoint[glyphs.Length];
        var advance = 0.0;
        for (var i = 0; i < glyphs.Length; i++)
        {
            var offset = run.GlyphOffsets == null || run.GlyphOffsets.Count == 0 ? new Point() : run.GlyphOffsets[i];
            // RTL 字形以右边为起点，使用字体本身的 advance 还原左侧字形原点。
            var x = (run.BidiLevel & 1) == 0 ? advance + offset.X
                : -advance - typeface.AdvanceWidths[glyphs[i]] * run.FontRenderingEmSize - offset.X;
            positions[i] = new SKPoint((float)(run.BaselineOrigin.X + x), (float)(run.BaselineOrigin.Y - offset.Y));
            advance += run.AdvanceWidths[i];
        }
        using var builder = new SKTextBlobBuilder();
        builder.AddPositionedRun(glyphs, font, positions);
        using var blob = builder.Build();
        var brush = glyphDrawing.ForegroundBrush as SolidColorBrush;
        var color = brush?.Color ?? Colors.White;
        using var paint = new SKPaint
        {
            Color = new SKColor(color.R, color.G, color.B, (byte)Math.Round(color.A * (brush?.Opacity ?? 1))),
            IsAntialias = true,
        };
        if (TextShadow is { } shadow)
        {
            var c = shadow.Color;
            using var filter = SKImageFilter.CreateDropShadowOnly(0, 0, (float)(shadow.BlurRadius / 3),
                (float)(shadow.BlurRadius / 3), new SKColor(c.R, c.G, c.B, (byte)Math.Round(c.A * shadow.Opacity)));
            paint.ImageFilter = filter;
            canvas.DrawText(blob, 0, 0, paint);
            paint.ImageFilter = null;
        }
        // 正文单独绘制，保持 Skia 的字形抗锯齿，不让阴影滤镜处理正文。
        canvas.DrawText(blob, 0, 0, paint);
    }
}
