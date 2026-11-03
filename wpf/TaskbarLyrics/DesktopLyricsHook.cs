// 只消费完整可见帧的文字与实测高亮，避免把预加载文字当作当前句。
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace TaskbarLyrics;

internal sealed record DesktopLyricsFrame(string Original, string Translation, double? Progress)
{
    public double? ObservedProgress { get; init; }
}

internal sealed class DesktopLyricsDraws
{
    private sealed class Row(string text, float y)
    {
        public string Text = text;
        public float Y = y;
        public uint Tick;
        public double? Progress;
        public double LastRatio;
        public int Advances;
        public uint ChangedAt;
    }
    private readonly List<Row> _rows = new();
    private readonly List<Row> _pendingRows = new();
    private Row[] _visibleRows = Array.Empty<Row>();
    private Row? _current;
    private Row? _next;
    private bool _holdCurrent;

    public void AddVisibleRow(string text, uint tick, float y, float width, float height, float highlightedWidth)
    {
        if (string.IsNullOrWhiteSpace(text) || !float.IsFinite(y + width + height + highlightedWidth)
            || width <= 0 || height <= 0 || highlightedWidth < 0) return;
        var row = _rows.FirstOrDefault(r => r.Text == text && Math.Abs(r.Y - y) < 2);
        if (row == null)
        {
            _rows.RemoveAll(r => Math.Abs(r.Y - y) < 2);
            row = new Row(text, y);
            _rows.Add(row);
        }
        row.Tick = tick;
        if (!_pendingRows.Contains(row)) _pendingRows.Add(row);
        var ratio = Math.Clamp(highlightedWidth / width, 0, 1);
        if (Math.Abs(ratio - row.LastRatio) > 0.002) row.ChangedAt = tick;
        // 观察持续推进，不能把一帧颜色噪点或整句常亮当作逐字。
        if (ratio < row.LastRatio - 0.1)
        {
            row.Advances = 0;
            row.Progress = null;
        }
        if (ratio > row.LastRatio + 0.002 && ratio - row.LastRatio < 0.2) row.Advances++;
        row.LastRatio = ratio;
        if (row.Advances >= 3) row.Progress = ratio;
    }

    public DesktopLyricsFrame? Frame()
    {
        var rows = _visibleRows;
        if (rows.Length == 0) return null;
        var latest = rows[0].Tick;
        // 无符号差值允许 GetTickCount 自然回绕，比较本帧各行最近的进度变化。
        foreach (var row in rows)
            if (unchecked((int)(row.Tick - latest)) > 0) latest = row.Tick;
        // 网易云双行可能是上下交替的当前句/下一句，不能固定选上行。
        var moving = rows.Where(r => r.LastRatio > 0 && r.LastRatio < 0.995).ToArray();
        var current = _current != null && rows.Contains(_current) ? _current : null;
        var hold = current != null && _holdCurrent;
        // 当前行被预加载的新句替换时，先接上已知下一句，不能跳到新出现的再下一句。
        if (current == null && _next != null && rows.Contains(_next)) { current = _next; hold = true; }
        if (current == null && moving.Length > 0)
            current = moving.OrderByDescending(r => unchecked((int)(r.ChangedAt - latest))).First();
        if (current == null || (!hold && current.LastRatio == 0))
            current = rows.Where(r => r.LastRatio > 0).OrderByDescending(r => unchecked((int)(r.ChangedAt - latest))).FirstOrDefault() ?? current;
        current ??= rows[0];
        // 两行都有局部着色时保持当前句，只有唱完才允许交给另一行。
        if (current.LastRatio >= 0.995)
            current = rows.FirstOrDefault(r => r != current && r.LastRatio > 0 && r.LastRatio < 0.995) ?? current;
        _current = current;
        _holdCurrent = hold || current.LastRatio > 0;
        var other = rows.FirstOrDefault(r => r != current);
        _next = other;
        return new(current.Text, other?.Text ?? "", current.Progress) { ObservedProgress = current.LastRatio };
    }

    public void CommitVisibleFrame(int expectedRows, uint tick)
    {
        var rows = _pendingRows.OrderBy(r => r.Y).Take(2).ToList();
        // 两行仍可见时，短暂匹配失败不等于用户切成了单行。
        if (expectedRows >= 2 && rows.Count == 1)
            foreach (var previous in _visibleRows)
                if (Math.Abs(previous.Y - rows[0].Y) >= 2 && unchecked(tick - previous.Tick) <= 350)
                { rows.Add(previous); break; }
        _visibleRows = rows.OrderBy(r => r.Y).ToArray();
        _pendingRows.Clear();
        if (tick != 0)
            _rows.RemoveAll(r => r != _current && r != _next && !_visibleRows.Contains(r)
                && unchecked(tick - r.Tick) > 1000);
    }
}

internal sealed class DesktopLyricsHook
{
    private const int ProtocolVersion = 19;
    private const int SlotSize = 1076, SlotCount = 256, HeaderSize = 16;
    private static string ChannelName(int pid) => $"Local\\TaskbarLyrics.Draw.v{ProtocolVersion}.{pid}";
    internal static bool TraceDrawCalls { get; set; }
    private volatile DesktopLyricsFrame? _frame;
    private volatile bool _connected;
    private volatile string _status = "未启用";
    private int _textSamples, _fillSamples, _pixelSamples;
    private volatile string _lastFill = "无";
    private volatile string _pixelInfo = "无";
    private bool _diagnostics;
    public DesktopLyricsFrame? Frame => _frame;
    public bool IsConnected => _connected;
    public string Status => _status;

    internal static async Task<int> RunConsoleTestAsync(int seconds)
    {
        var pid = FindPlayer();
        using var snapshot = pid > 0 ? MemoryMappedFile.CreateOrOpen(ChannelName(pid) + ".snapshot", 16 + 4096 * 512 * 4) : null;
        using var snapshotView = snapshot?.CreateViewAccessor();
        snapshotView?.Write(0, 1);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(Math.Clamp(seconds, 1, 120)));
        var hook = new DesktopLyricsHook { _diagnostics = true };
        var task = Task.Run(() => hook.RunAsync(() => true, timeout.Token));
        var previous = "";
        var captured = false;
        var refreshAt = DateTime.UtcNow.AddSeconds(Math.Max(1, Math.Clamp(seconds, 1, 120) - 2));
        var refreshed = false;
        while (!timeout.IsCancellationRequested)
        {
            var frame = hook.Frame;
            if (!refreshed && DateTime.UtcNow >= refreshAt)
            {
                snapshotView?.Write(0, 1);
                refreshed = true;
            }
            captured |= frame != null;
            var line = frame == null ? hook.Status : $"{frame.Original} | {frame.Translation} | 高亮={frame.Progress?.ToString("P1") ?? "未知"}";
            if (line != previous) { Console.WriteLine(line); previous = line; }
            try { await Task.Delay(50, timeout.Token); }
            catch (OperationCanceledException) { break; }
        }
        await task;
        Console.WriteLine($"采样：文字={hook._textSamples}，填充={hook._fillSamples}，最终高亮={hook._pixelSamples}；最后采样={hook._lastFill}");
        Console.WriteLine("绘制区域：" + hook._pixelInfo);
        if (snapshotView?.ReadInt32(0) == 2)
        {
            var width = snapshotView.ReadInt32(4); var height = snapshotView.ReadInt32(8);
            if (width > 0 && width <= 4096 && height > 0 && height <= 512)
            {
                var pixels = new byte[width * height * 4];
                snapshotView.ReadArray(16, pixels, 0, pixels.Length);
                using var bitmap = new SkiaSharp.SKBitmap(width, height, SkiaSharp.SKColorType.Bgra8888, SkiaSharp.SKAlphaType.Premul);
                Marshal.Copy(pixels, 0, bitmap.GetPixels(), pixels.Length);
                using var png = bitmap.Encode(SkiaSharp.SKEncodedImageFormat.Png, 100);
                var path = Path.Combine(AppContext.BaseDirectory, "hook-diagnostic.png");
                using var output = File.Create(path); png.SaveTo(output);
                Console.WriteLine("绘制图像：" + path);
            }
        }
        return captured ? 0 : 1;
    }

    private delegate bool WindowCallback(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr data);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int length);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out int pid);

    private static int FindPlayer()
    {
        var result = 0;
        EnumWindows((window, _) =>
        {
            var name = new StringBuilder(64);
            GetClassName(window, name, name.Capacity);
            if (name.ToString() != "DesktopLyrics") return true;
            GetWindowThreadProcessId(window, out var pid);
            try
            {
                using var process = Process.GetProcessById(pid);
                if (!process.ProcessName.Equals("cloudmusic", StringComparison.OrdinalIgnoreCase)) return true;
                result = pid;
                return false;
            }
            catch (ArgumentException) { return true; }
        }, IntPtr.Zero);
        return result;
    }

    private static string Extract(string resource, string fileName)
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream(resource)
            ?? throw new IOException("缺少原生 Hook 资源");
        using var buffer = new MemoryStream();
        stream.CopyTo(buffer);
        var bytes = buffer.ToArray();
        var digest = Convert.ToHexString(SHA256.HashData(bytes));
        // 部分网易云客户端拒绝加载 Roaming 目录内的 DLL。优先解包到程序旁的独立缓存。
        var directory = Path.Combine(AppContext.BaseDirectory, ".lyrics-hook", digest);
        try { Directory.CreateDirectory(directory); }
        catch (UnauthorizedAccessException)
        {
            directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TaskbarLyrics", "hooks", digest);
            Directory.CreateDirectory(directory);
        }
        var path = Path.Combine(directory, fileName);
        if (!File.Exists(path) || Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))) != digest)
            File.WriteAllBytes(path, bytes);
        return path;
    }

    public async Task RunAsync(Func<bool> enabled, CancellationToken token)
    {
        using var traceLog = TraceDrawCalls ? File.CreateText(Path.Combine(AppContext.BaseDirectory, "hook-draw-trace.log")) : null;
        traceLog?.WriteLine("Draw trace: text/cache/copy/frame links; display selection remains unchanged.");
        var attemptedPid = 0;
        var attemptedAt = double.NegativeInfinity;
        while (!token.IsCancellationRequested)
        {
            if (!enabled()) { _frame = null; _status = "未启用"; }
            else
            {
                var pid = FindPlayer();
                if (pid == 0) { _frame = null; _status = "请开启网易云桌面歌词（需要 64 位客户端）"; }
                else
                {
                    try
                    {
                        var name = ChannelName(pid);
                        using var mapping = MemoryMappedFile.CreateOrOpen(name, HeaderSize + SlotSize * SlotCount);
                        using var view = mapping.CreateViewAccessor();
                        using var signal = new EventWaitHandle(false, EventResetMode.AutoReset, name + ".event");
                        view.Write(4, TraceDrawCalls ? 3 : 1);
                        try
                        {
                            if (view.ReadInt32(12) != 1 && (pid != attemptedPid || Clock.Now - attemptedAt >= 60))
                            {
                                attemptedPid = pid;
                                attemptedAt = Clock.Now;
                                _status = "正在连接网易云桌面歌词";
                                var dll = Extract("LyricsHook.dll", $"TaskbarLyricsHook{ProtocolVersion}.dll");
                                var injector = Extract("LyricsHookInjector.exe", "LyricsHookInjector.exe");
                                var start = new ProcessStartInfo(injector) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true };
                                start.ArgumentList.Add(pid.ToString());
                                start.ArgumentList.Add(dll);
                                using var process = Process.Start(start) ?? throw new IOException("注入器启动失败");
                                var errorTask = process.StandardError.ReadToEndAsync(token);
                                await process.WaitForExitAsync(token);
                                var error = (await errorTask).Trim();
                                if (process.ExitCode != 0)
                                    _status = $"Hook 连接失败（{process.ExitCode}） {error}，请检查客户端位数与权限";
                            }
                            var draws = new DesktopLyricsDraws();
                            var seen = traceLog != null ? 0 : view.ReadInt32(8);
                            var lastProbe = Clock.Now;
                            var sample = new byte[SlotSize];
                            _connected = true;
                            while (enabled() && !token.IsCancellationRequested)
                            {
                                signal.WaitOne(16);
                                if (Clock.Now - lastProbe > 1)
                                {
                                    lastProbe = Clock.Now;
                                    if (FindPlayer() != pid) break;
                                    traceLog?.Flush();
                                    var ready = view.ReadInt32(12);
                                    if (ready < 0) { _status = $"原生 Hook 安装失败（阶段 {-ready}）"; break; }
                                    if (ready != 1) break;
                                }
                                var latest = view.ReadInt32(8);
                                if (latest <= seen) continue;
                                if (latest - seen > SlotCount) {
                                    traceLog?.WriteLine($"DROPPED {latest - seen - SlotCount} packets");
                                    seen = latest - SlotCount;
                                }
                                while (seen < latest)
                                {
                                    var sequence = ++seen;
                                    var offset = HeaderSize + (sequence - 1) % SlotCount * SlotSize;
                                    var before = view.ReadInt32(offset);
                                    if (before != sequence * 2) continue;
                                    view.ReadArray(offset, sample, 0, sample.Length);
                                    Thread.MemoryBarrier();
                                    if (before != view.ReadInt32(offset)) continue;
                                    if (BitConverter.ToInt32(sample, 4) != sequence) continue;
                                    var kind = BitConverter.ToUInt32(sample, 16);
                                    if (kind == 8)
                                    {
                                        draws.CommitVisibleFrame((int)(BitConverter.ToUInt32(sample, 12) >> 16), BitConverter.ToUInt32(sample, 8));
                                        _frame = draws.Frame();
                                        continue;
                                    }
                                    if (kind != 4 && !_diagnostics && !(kind == 9 && traceLog != null)) continue;
                                    var length = 0;
                                    while (length < 1024 && (sample[52 + length] != 0 || sample[53 + length] != 0)) length += 2;
                                    var text = Encoding.Unicode.GetString(sample, 52, length);
                                    if (kind == 9) {
                                        var bounds = FormattableString.Invariant($"dst=({BitConverter.ToSingle(sample, 20)},{BitConverter.ToSingle(sample, 24)},{BitConverter.ToSingle(sample, 28)},{BitConverter.ToSingle(sample, 32)})");
                                        var clip = FormattableString.Invariant($"src=({BitConverter.ToSingle(sample, 36)},{BitConverter.ToSingle(sample, 40)},{BitConverter.ToSingle(sample, 44)},{BitConverter.ToSingle(sample, 48)})");
                                        traceLog?.WriteLine($"{BitConverter.ToUInt32(sample, 8)} {text} {bounds} {clip}");
                                        continue;
                                    }
                                    if (kind == 7) { if (_diagnostics) _pixelInfo = text; continue; }
                                    if (kind == 0) _textSamples++;
                                    else if (_diagnostics)
                                    {
                                        if (kind == 4) _pixelSamples++;
                                        else _fillSamples++;
                                        _lastFill = $"kind={kind}, color={BitConverter.ToUInt32(sample, 12):X8}, path=({BitConverter.ToSingle(sample, 20):F1},{BitConverter.ToSingle(sample, 24):F1},{BitConverter.ToSingle(sample, 28):F1},{BitConverter.ToSingle(sample, 32):F1}), clip=({BitConverter.ToSingle(sample, 36):F1},{BitConverter.ToSingle(sample, 40):F1},{BitConverter.ToSingle(sample, 44):F1},{BitConverter.ToSingle(sample, 48):F1})";
                                    }
                                    // 路径事件含有预先生成的下一句，只用最终可见图像决定显示文字。
                                    if (kind != 4) continue;
                                    draws.AddVisibleRow(text, BitConverter.ToUInt32(sample, 8), BitConverter.ToSingle(sample, 24),
                                        BitConverter.ToSingle(sample, 28), BitConverter.ToSingle(sample, 32),
                                        BitConverter.ToSingle(sample, 44));
                                }
                                _status = _frame?.Progress.HasValue == true ? "已捕获文字和实际高亮位置"
                                    : "已取词，未识别高亮进度";
                            }
                        }
                        finally
                        {
                            // 先撤销旧帧，再通知显示层清空；短暂缺帧仍由连接状态区分。
                            _frame = null;
                            _connected = false;
                            view.Write(4, 0);
                        }
                    }
                    catch (OperationCanceledException) when (token.IsCancellationRequested) { break; }
                    catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
                    { _status = "Hook 连接失败：" + ex.Message; _frame = null; }
                }
            }
            try { await Task.Delay(1000, token); }
            catch (OperationCanceledException) { break; }
        }
    }
}
