// 离线回归：构造窗口和播放数据，不连接播放器，不修改配置或注册表。
using System.Reflection;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Media.Animation;
using System.IO;
using TaskbarLyrics;

static class Program
{
    static readonly Type DrawsType = typeof(AppConfig).Assembly.GetType("TaskbarLyrics.DesktopLyricsDraws")!;
    static object Draws() => Activator.CreateInstance(DrawsType, true)!;
    static void VisibleRow(object draws, string text, uint tick, float y, float highlightedWidth)
        => DrawsType.GetMethod("AddVisibleRow")!.Invoke(draws, [text, tick, y, 100f, 20f, highlightedWidth]);
    static void CommitVisible(object draws, int expectedRows, uint tick)
        => DrawsType.GetMethod("CommitVisibleFrame")!.Invoke(draws, [expectedRows, tick]);
    static object? Frame(object draws) => DrawsType.GetMethod("Frame")!.Invoke(draws, null);
    static T? Property<T>(object frame, string name) => (T?)frame.GetType().GetProperty(name)!.GetValue(frame);
    static void Check(bool condition, string name) { if (!condition) throw new Exception(name); Console.WriteLine("PASS " + name); }

    [STAThread]
    static void Main()
    {
        Check(new AppConfig().LyricsSync == "timeline", "默认仍为原有时间轴");
        foreach (var mode in new[] { "timeline", "netease_hook" }) {
            var config = new AppConfig { LyricsSync = mode, LyricsRenderer = "wpf", SettingsRenderer = "skia" };
            var copy = JsonSerializer.Deserialize<AppConfig>(JsonSerializer.Serialize(config))!;
            Check(copy.LyricsSync == mode && copy.LyricsRenderer == "wpf" && copy.SettingsRenderer == "skia", "同步方案和渲染引擎独立持久化 " + mode);
        }
        var pixels = Draws();
        void Pixel(float width) { VisibleRow(pixels, "像素", 100, 10, width); CommitVisible(pixels, 1, 100); }
        Pixel(100);
        Check(Property<double?>(Frame(pixels)!, "Progress") == null, "整句常亮不冒充逐字");
        Pixel(25);
        Check(Property<double?>(Frame(pixels)!, "Progress") == null, "单帧比例不冒充可靠逐字");
        Pixel(30); Pixel(35); Pixel(40);
        Check(Math.Abs(Property<double?>(Frame(pixels)!, "Progress")!.Value - .4) < .001, "单行连续推进后采用真实边界");
        Check(Property<string>(Frame(pixels)!, "Translation") == "", "单行捕获不等待第二行");
        Pixel(100);
        Check(Property<double?>(Frame(pixels)!, "Progress") == 1, "实际逐字唱满");
        var midway = Draws();
        foreach (var width in new[] { 70f, 72f, 74f, 76f }) {
            VisibleRow(midway, "中途接入", 100, 10, width);
            CommitVisible(midway, 1, 100);
        }
        Check(Property<double?>(Frame(midway)!, "Progress") > .75, "中途接入也采用实测，不从匀速重新起扫");
        var alternating = Draws();
        uint frameTick = 100;
        void Visible(string text, float y, float ratio) => VisibleRow(alternating, text, frameTick, y, ratio * 100);
        void Commit() => CommitVisible(alternating, 0, frameTick);
        Visible("上句", 10, .4f); Visible("下句", 40, 0); Commit();
        Check(Property<string>(Frame(alternating)!, "Original") == "上句", "双行上句正在唱时选上句");
        frameTick = 200;
        Visible("上句", 10, 1); Visible("下句", 40, .2f); Commit();
        Check(Property<string>(Frame(alternating)!, "Original") == "下句", "双行轮到下句时正确切换");
        frameTick = 300;
        Visible("下句", 40, .3f); Commit();
        Check(Property<string>(Frame(alternating)!, "Translation") == "", "完整帧提交不残留旧的另一行");
        frameTick = 400; Commit();
        Check(Frame(alternating) == null, "未匹配可见文字时不复用旧行");
        var retained = Draws();
        void RetainRow(string text, float y, uint time, float ratio)
            => VisibleRow(retained, text, time, y, ratio * 100);
        void CommitRows(int count, uint time) => DrawsType.GetMethod("CommitVisibleFrame")!.Invoke(retained, [count, time]);
        RetainRow("上行", 10, 100, .4f); RetainRow("下行", 40, 100, 0); CommitRows(2, 100);
        RetainRow("上行", 10, 140, .5f); CommitRows(2, 140);
        Check(Property<string>(Frame(retained)!, "Translation") == "下行", "双行一帧匹配失败时保留另一行");
        RetainRow("上行", 10, 200, .6f); CommitRows(1, 200);
        Check(Property<string>(Frame(retained)!, "Translation") == "", "真正切成单行时立即移除旧第二行");
        RetainRow("下行", 40, 210, .1f); CommitRows(2, 210);
        Check(Property<string>(Frame(retained)!, "Original") == "上行", "当前句匹配短暂丢失时不抢到下句");
        RetainRow("下行", 40, 700, .3f); CommitRows(2, 700);
        Check(Property<string>(Frame(retained)!, "Original") == "下行", "过期旧行不阻挡新行");
        var sequence = Draws();
        uint sequenceTick = 0;
        string Pair(string upper, float a, string lower, float b)
        {
            sequenceTick += 100;
            foreach (var item in new[] { (upper, 10f, a), (lower, 40f, b) })
                VisibleRow(sequence, item.Item1, sequenceTick, item.Item2, item.Item3 * 100);
            CommitVisible(sequence, 0, sequenceTick);
            return Property<string>(Frame(sequence)!, "Original")!;
        }
        var order = new[] { Pair("1", .2f, "2", 0), Pair("1", 1, "2", .2f),
            Pair("3", 0, "2", .5f), Pair("3", .1f, "4", .3f), Pair("3", .4f, "4", .45f),
            Pair("3", 1, "4", .2f), Pair("5", 0, "4", .7f), Pair("5", .1f, "6", 0) };
        Check(string.Join(",", order) == "1,2,2,3,3,4,4,5", "逐字双行预加载不产生 1-2-4-3 跳句");
        var noisy = Draws();
        VisibleRow(noisy, "当前句", 100, 10, 10);
        VisibleRow(noisy, "下一句", 100, 40, 0);
        CommitVisible(noisy, 2, 100);
        Check(Property<string>(Frame(noisy)!, "Original") == "当前句", "待唱行进度为零时不抢当前句");

        var app = new TaskbarLyrics.App(); app.InitializeComponent();
        using var controller = new MainController();
        Check(new AppConfig().SecondLine == "translation", "第二行默认仍为译文");
        foreach (var second in new[] { "translation", "romaji", "next", "off" }) {
            controller.Cfg.SecondLine = second;
            var window = new SettingsWindow(controller, 1);
            var combo = (ComboBox)window.FindName("SecondLineCombo");
            Check(combo.Items.Count == 4 && combo.SelectedIndex == Array.IndexOf(new[] { "translation", "romaji", "next", "off" }, second), "第二行选项回填 " + second);
            var roundtrip = JsonSerializer.Deserialize<AppConfig>(JsonSerializer.Serialize(new AppConfig { SecondLine = second }))!;
            Check(roundtrip.SecondLine == second, "第二行配置持久化 " + second);
            window.Close();
        }
        controller.Cfg.SecondLine = "translation";
        foreach (var mode in new[] { "timeline", "netease_hook" }) {
            controller.Cfg.LyricsSync = mode;
            var window = new SettingsWindow(controller, 1);
            Check(((ComboBox)window.FindName("LyricsSyncCombo")).SelectedIndex == (mode == "timeline" ? 0 : 1), "设置回填 " + mode);
            if (mode == "netease_hook") {
                window.ShowActivated = false; window.ShowInTaskbar = false; window.Opacity = 0;
                window.Show(); window.UpdateLayout();
                var content = (FrameworkElement)window.Content;
                var bitmap = new RenderTargetBitmap((int)content.ActualWidth, (int)content.ActualHeight, 96, 96, PixelFormats.Pbgra32);
                bitmap.Render(content);
                var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(bitmap));
                using var image = File.Create(Path.Combine(AppContext.BaseDirectory, "settings-hook.png")); encoder.Save(image);
            }
            window.Close();
        }
        foreach (var engine in new[] { "skia", "wpf" }) {
            var text = new KaraokeText();
            SkiaTextBlock.SetRenderer(text, engine);
            text.SetLine("歌词进度测试", null, new FontFamily("Microsoft YaHei UI"), 16, Brushes.White, Brushes.Gray, false, false);
            text.Measure(new Size(500, 100)); text.Arrange(new Rect(0, 0, 500, 100));
            var set = typeof(KaraokeText).GetMethod("SetCapturedProgress", BindingFlags.Instance | BindingFlags.NonPublic)!;
            var accent = (FrameworkElement)typeof(KaraokeText).GetField("_accent", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(text)!;
            foreach (var fraction in new[] { .25, .75 }) {
                set.Invoke(text, [fraction]);
                var clip = (RectangleGeometry)accent.Clip;
                Check(Math.Abs(clip.Rect.Width - text.MeasureLineWidth() * fraction) < .01, engine + " 捕获比例用于实际高亮裁剪");
            }
            set.Invoke(text, [null]);
            Check(accent.Clip == null, engine + " 未知进度恢复整句显示");
        }
        controller.Cfg.Mode = "floating";
        controller.Cfg.ShowGuide = false;
        controller.Cfg.LyricsSync = "netease_hook";
        controller.Cfg.Karaoke = true;
        var overlay = new OverlayWindow(controller) { ShowActivated = false, ShowInTaskbar = false, Opacity = 0 };
        overlay.Show();
        var flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var main = typeof(MainController);
        main.GetField("_overlay", flags)!.SetValue(controller, overlay);
        var playback = new PlaybackState { Title = "模拟歌曲", Artist = "测试", SourceId = "cloudmusic.exe", Playing = false, BasePositionS = 900 };
        main.GetField("_state", flags)!.SetValue(controller, playback);
        main.GetField("_lines", flags)!.SetValue(controller, new List<LyricLine> { new(0, "原有歌词", null) });
        var hook = main.GetField("_desktopHook", flags)!.GetValue(controller)!;
        var frameType = typeof(AppConfig).Assembly.GetType("TaskbarLyrics.DesktopLyricsFrame")!;
        void SetFrame(double? progress, double? observed = 1)
        {
            var frame = Activator.CreateInstance(frameType, "Hook 歌词", "译文", progress)!;
            frameType.GetProperty("ObservedProgress")!.SetValue(frame, progress ?? observed);
            hook.GetType().GetField("_frame", flags)!.SetValue(hook, frame);
        }
        var tick = main.GetMethod("OnLyricsTick", flags)!;
        var lineField = typeof(OverlayWindow).GetField("_currentLine", flags)!;
        double HighlightFraction()
        {
            var karaoke = (KaraokeText?)typeof(OverlayWindow).GetField("_karaoke", flags)!.GetValue(overlay);
            if (karaoke == null) return 0;
            var accent = (FrameworkElement)typeof(KaraokeText).GetField("_accent", flags)!.GetValue(karaoke)!;
            return accent.Clip is RectangleGeometry clip ? clip.Rect.Width / Math.Max(1, karaoke.MeasureLineWidth()) : 1;
        }
        var storyboardField = typeof(OverlayWindow).GetField("_karaokeStoryboard", flags)!;
        SetFrame(null, 0); tick.Invoke(controller, null);
        Check(HighlightFraction() == 0, "等待逐字时保持未唱，不偷跑");
        Check(storyboardField.GetValue(overlay) == null, "Hook 不生成任何匀速动画");
        SetFrame(null, 1); tick.Invoke(controller, null);
        Check((string)main.GetField("_shownOriginal", flags)!.GetValue(controller)! == "Hook 歌词", "无逐字时仍显示 Hook 文字");
        Check(HighlightFraction() == 1, "默认全行着色的无逐字歌词整行显示");
        Check(storyboardField.GetValue(overlay) == null, "无逐字不合成扫过效果");
        var originalVisual = lineField.GetValue(overlay);
        SetFrame(.4); tick.Invoke(controller, null);
        Check(ReferenceEquals(originalVisual, lineField.GetValue(overlay)), "实测接管不重建当前行");
        Check(Math.Abs(HighlightFraction() - .4) < .001, "有逐字时严格跟随实测");
        SetFrame(null); tick.Invoke(controller, null);
        Check(Math.Abs(HighlightFraction() - .4) < .001, "已确认逐字的数据短暂中断时不启动估算");
        hook.GetType().GetField("_frame", flags)!.SetValue(hook, Activator.CreateInstance(frameType, "单行新歌词", "", null));
        tick.Invoke(controller, null);
        Check((string)main.GetField("_shownOriginal", flags)!.GetValue(controller)! == "单行新歌词"
            && (string)main.GetField("_shownTranslation", flags)!.GetValue(controller)! == "", "单行网易云歌词可独立显示");
        Check(storyboardField.GetValue(overlay) == null, "无逐字的新句不生成扫过动画");
        var host = (Panel)overlay.FindName("LinesHost");
        Check(host.Children.Count >= 2, "真正换句时保留旧行参与切换动画");
        var transitioningLine = lineField.GetValue(overlay);
        hook.GetType().GetField("_frame", flags)!.SetValue(hook, Activator.CreateInstance(frameType, "单行新歌词", "下一句稍后到达", .02));
        tick.Invoke(controller, null);
        Check(ReferenceEquals(transitioningLine, lineField.GetValue(overlay)) && host.Children.Count >= 2,
            "进度接管和第二行更新不会清掉正在播放的换句动画");
        hook.GetType().GetField("_frame", flags)!.SetValue(hook, null);
        tick.Invoke(controller, null);
        Check((string)main.GetField("_shownOriginal", flags)!.GetValue(controller)! == "单行新歌词", "暂时未匹配到时保留上一句");
        Check(!(bool)typeof(OverlayWindow).GetField("_infoMode", flags)!.GetValue(overlay)!, "歌词条不显示诊断占位信息");
        main.GetField("_hookLineActive", flags)!.SetValue(controller, false);
        main.GetField("_state", flags)!.SetValue(controller, null);
        tick.Invoke(controller, null);
        Check((string)typeof(OverlayWindow).GetField("_title", flags)!.GetValue(overlay)! == ""
            && (string)typeof(OverlayWindow).GetField("_artist", flags)!.GetValue(overlay)! == "", "尚无歌词时保持空白，不显示连接状态");
        main.GetField("_state", flags)!.SetValue(controller, playback);
        controller.Cfg.LyricsSync = "timeline"; SetFrame(.8); tick.Invoke(controller, null);
        Check((string)main.GetField("_shownOriginal", flags)!.GetValue(controller)! == "原有歌词", "手动选择原有方案后不受 Hook 影响");
        playback.BasePositionS = 0;
        main.GetField("_lines", flags)!.SetValue(controller, new List<LyricLine> { new(0, "当前原文", "当前译文"), new(1000, "下一句原文", "下一句译文") });
        foreach (var second in new[] { "translation", "next" }) {
            controller.Cfg.SecondLine = second;
            tick.Invoke(controller, null);
            Check((string)main.GetField("_shownTranslation", flags)!.GetValue(controller)! == (second == "next" ? "下一句原文" : "当前译文"), "有译文时第二行选择 " + second);
        }
        playback.BasePositionS = 1;
        tick.Invoke(controller, null);
        Check((string)main.GetField("_shownTranslation", flags)!.GetValue(controller)! == "", "下一句模式末句留空，不回退译文");
        playback.BasePositionS = 0;
        controller.Cfg.SecondLine = "translation";
        main.GetField("_lines", flags)!.SetValue(controller, new List<LyricLine> { new(0, "当前原文", null), new(1000, "下一句原文", null) });
        tick.Invoke(controller, null);
        Check((string)main.GetField("_shownTranslation", flags)!.GetValue(controller)! == "下一句原文", "默认译文缺失时仍自动显示下一句");
        overlay.Close();
        app.Shutdown();
    }
}
