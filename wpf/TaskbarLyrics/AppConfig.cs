// 配置读写：config.json 与 Python 版同 schema（移植自 settings.py），存用户 AppData 目录。
using System.IO;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace TaskbarLyrics;

public sealed class AppConfig
{
    // 外观
    [JsonPropertyName("font_family")] public string FontFamily { get; set; } = "Microsoft YaHei UI";
    [JsonPropertyName("font_size")] public int FontSize { get; set; } = 12;            // 原文字号
    [JsonPropertyName("font_bold")] public bool FontBold { get; set; }               // 原文半粗（译文保持常规）
    [JsonPropertyName("lyrics_renderer")] public string LyricsRenderer { get; set; } = "skia"; // skia | wpf
    [JsonPropertyName("settings_renderer")] public string SettingsRenderer { get; set; } = "skia";
    [JsonPropertyName("lyrics_hinting")] public string LyricsHinting { get; set; } = "normal"; // none | slight | normal | full
    [JsonPropertyName("settings_hinting")] public string SettingsHinting { get; set; } = "normal";
    // auto：跟随系统任务栏明暗自动取色（深色任务栏白字、浅色任务栏近黑字）；
    // custom：用下面两个 hex。默认 auto——浅色任务栏上白字几乎看不见，
    // 而绝大多数人不会主动去设置里改颜色，默认就得是对的
    [JsonPropertyName("text_color_mode")] public string TextColorMode { get; set; } = "auto";
    [JsonPropertyName("text_color")] public string TextColor { get; set; } = "#FFFFFF";
    [JsonPropertyName("trans_color")] public string TransColor { get; set; } = "#C8C8C8";
    [JsonPropertyName("shadow")] public bool Shadow { get; set; } = true;              // 文字阴影
    [JsonPropertyName("width")] public int Width { get; set; } = 280;                  // 歌词区最大宽度（紧凑布局下内容更窄时收缩，超出才缩字号）
    [JsonPropertyName("text_align")] public string TextAlign { get; set; } = "left"; // center | left（配封面时左对齐更整齐）
    [JsonPropertyName("show_controls")] public bool ShowControls { get; set; } = true; // 悬停播放控制按钮
    // 位置
    [JsonPropertyName("mode")] public string Mode { get; set; } = "taskbar";           // taskbar | floating
    [JsonPropertyName("position")] public string Position { get; set; } = "custom";    // tray_left | left | center | right | custom
    [JsonPropertyName("auto_position")] public bool AutoPosition { get; set; } = true; // 自动避让任务栏元素（覆盖 position）
    [JsonPropertyName("auto_side")] public string AutoSide { get; set; } = "right";    // 避让停靠侧：left | right（优先待在哪半边）
    [JsonPropertyName("auto_align")] public string AutoAlign { get; set; } = "left";   // 空档内停靠对齐：left | right | center
    [JsonPropertyName("x_offset")] public int? XOffset { get; set; }                   // position=custom 时任务栏内 x（左缘锚点）
    [JsonPropertyName("x_center")] public int? XCenter { get; set; }                   // 居中对齐时的中心锚点（任务栏内 x）
    [JsonPropertyName("float_x")] public int? FloatX { get; set; }                     // 浮动模式屏幕坐标（左缘锚点）
    [JsonPropertyName("float_cx")] public int? FloatCx { get; set; }                   // 居中对齐时浮动窗中心锚点（屏幕坐标）
    [JsonPropertyName("float_y")] public int? FloatY { get; set; }
    [JsonPropertyName("monitor")] public int Monitor { get; set; } = 0;
    [JsonPropertyName("locked")] public bool Locked { get; set; }                      // 锁定后鼠标穿透
    [JsonPropertyName("show_cover")] public bool ShowCover { get; set; } = true;       // 显示专辑封面
    // 歌词
    [JsonPropertyName("lyrics_sync")] public string LyricsSync { get; set; } = "timeline"; // timeline | netease_hook
    [JsonPropertyName("second_line")] public string SecondLine { get; set; } = "translation"; // translation | romaji | off
    [JsonPropertyName("karaoke")] public bool Karaoke { get; set; } = true;            // 逐字歌词
    [JsonPropertyName("offset_ms")] public int OffsetMs { get; set; } = 0;             // 歌词时间偏移（正为提前：posMs 加得多，查到更靠后的行）
    // 行为
    [JsonPropertyName("hide_on_fullscreen")] public bool HideOnFullscreen { get; set; } = true;
    [JsonPropertyName("stats_enabled")] public bool StatsEnabled { get; set; } = true;  // 记录听歌统计（只存本机）
    [JsonPropertyName("stats_images")] public bool StatsImages { get; set; } = true;    // 统计页配封面和头像（播放时顺带下载，存本机）
    // 更新
    [JsonPropertyName("update_check")] public bool UpdateCheck { get; set; } = true;   // 启动时自动检查新版本
    // 上次成功检查更新的 unix 秒。只用于给启动检查节流：GitHub 匿名接口每小时 60 次
    // 且按出口 IP 算，频繁重启（或共用出口的多台机器）会白白耗掉配额
    [JsonPropertyName("last_update_check")] public long LastUpdateCheck { get; set; }
    // 播放源
    [JsonPropertyName("player_source")] public string PlayerSource { get; set; } = "auto"; // auto | netease | others
    [JsonPropertyName("player_blocklist")] public List<string> PlayerBlocklist { get; set; } = new() { "chrome", "msedge", "firefox" };
    // 新手引导：首次运行（还没有配置文件）时由 Load 置上，放出第一首歌或用过一次右键菜单后清掉。
    // 默认 false：老用户的配置里没有这个键，升级后不该再看一遍引导
    [JsonPropertyName("show_guide")] public bool ShowGuide { get; set; }

    // 保留 Python 版里未知的扩展键，存盘时不丢
    [JsonExtensionData] public Dictionary<string, JsonElement>? Extra { get; set; }

    /// <summary>配置固定存用户目录，切换程序位置时仍共用同一份设置。</summary>
    public static string ConfigPath => FallbackPath;

    /// <summary>旧版配置位置，仅用于迁移和查找旧统计。</summary>
    internal static string PrimaryPath => Path.Combine(AppContext.BaseDirectory, "config.json");

    /// <summary>用户配置位置，保留原属性名供旧数据目录查找使用。</summary>
    internal static string FallbackPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "TaskbarLyrics", "config.json");

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping, // 对应 Python ensure_ascii=False
    };

    /// <summary>优先读用户配置；没有有效配置时迁移 exe 旁的旧配置，保留旧文件。
    /// 迁移写入失败时仍使用读到的设置，之后存盘继续尝试用户目录。</summary>
    public static AppConfig Load()
    {
        if (TryLoadFrom(ConfigPath, out var cfg)) return cfg;
        if (TryLoadFrom(PrimaryPath, out cfg))
        {
            cfg.Save();
            return cfg;
        }
        // 两处都没有有效配置时按首次运行处理。
        return new AppConfig { ShowGuide = true };
    }

    /// <summary>试着从一处读。文件不存在返回 false（不算故障）；
    /// 内容坏了则记日志并把它隔离成 .bad 再返回 false。
    ///
    /// 隔离这一步是必须的：解析失败原先只是静默回到默认值，而回默认只发生在内存里，
    /// 紧接着任何一次存盘（拖动窗口松手、检查更新写时间戳）就会把整份默认值
    /// 覆盖到原文件上，用户攒下的设置从此不可恢复。改名留证之后，
    /// 坏文件既不会被覆盖，也让下一次 Load 有机会退到另一处那份旧配置。</summary>
    private static bool TryLoadFrom(string path, out AppConfig cfg)
    {
        cfg = null!;
        if (!File.Exists(path)) return false;
        try
        {
            var node = JsonNode.Parse(File.ReadAllText(path))?.AsObject()
                ?? throw new InvalidDataException("配置内容不是 JSON 对象");
            // 旧配置迁移：show_translation 布尔 → second_line 枚举
            if (node.ContainsKey("show_translation") && !node.ContainsKey("second_line"))
            {
                var old = node["show_translation"];
                node["second_line"] = old is JsonValue v && v.TryGetValue<bool>(out var b) && b
                    ? "translation" : "off";
            }
            node.Remove("show_translation");
            // 旧的 WPF 渲染选项已由 Skia Hinting 替代，不作为扩展键继续写回。
            node.Remove("text_rendering");
            // 共用 Hinting 拆成歌词和设置窗口两份，已有独立配置时不覆盖。
            if (node.TryGetPropertyValue("font_hinting", out var hinting))
            {
                if (!node.ContainsKey("lyrics_hinting")) node["lyrics_hinting"] = hinting?.DeepClone();
                if (!node.ContainsKey("settings_hinting")) node["settings_hinting"] = hinting?.DeepClone();
                node.Remove("font_hinting");
            }
            cfg = node.Deserialize<AppConfig>()
                ?? throw new InvalidDataException("配置反序列化结果为空");
            return true;
        }
        catch (Exception ex)
        {
            // 不记的话，用户看到的只是「设置全变回默认了」，事后无从下手
            Log.Error("config-load", ex);
            try { File.Move(path, path + ".bad", overwrite: true); }
            catch { /* 挪不动就算了，日志里已经留了一条 */ }
            return false;
        }
    }

    private static bool _writeLogged;

    // UI 线程和后台线程都可能存盘，用静态锁保护同一配置文件的写入。
    private static readonly object SaveLock = new();

    /// <summary>存到用户目录，返回是否成功，调用方据此提示一次。</summary>
    public bool Save()
    {
        lock (SaveLock)
        {
            return TryWrite(ConfigPath);
        }
    }

    private bool TryWrite(string path)
    {
        // 临时文件名带上进程号：上面的锁只管得住本进程，而 --settings 单开的设置窗是另一个进程，
        // 跟常驻的那份同时存盘时照样会撞上同一个 .tmp
        var tmp = $"{path}.{Environment.ProcessId}.tmp";
        try
        {
            var dir = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
            // 先写临时文件再原子改名。直接覆写的话，写一半遇上关机/断电/杀进程
            // 就留下一份半截 JSON，而解析失败的代价是整份设置回默认值
            // （LyricsCache 那边踩过同一个坑，解法也是这个）
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, JsonOptions));
            File.Move(tmp, path, overwrite: true);
            return true;
        }
        catch (Exception ex)
        {
            // 名字不再固定，残留的临时文件不会被下一次写入覆盖掉，失败时顺手删掉
            try { File.Delete(tmp); }
            catch { /* 删不掉就算了 */ }
            // 只记第一次：拖动窗口每次松手都会存盘，失败时会把 error.log 刷满
            if (!_writeLogged)
            {
                _writeLogged = true;
                Log.Error("config-save", ex);
            }
            return false;
        }
    }
}
