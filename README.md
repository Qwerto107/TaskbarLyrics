# 任务栏歌词（Taskbar Lyrics）

[![release](https://img.shields.io/github/v/release/ChissaQAQ/TaskbarLyrics?label=release)](https://github.com/ChissaQAQ/TaskbarLyrics/releases/latest)
[![downloads](https://img.shields.io/github/downloads/ChissaQAQ/TaskbarLyrics/total?label=downloads)](https://github.com/ChissaQAQ/TaskbarLyrics/releases)
[![build](https://github.com/ChissaQAQ/TaskbarLyrics/actions/workflows/build.yml/badge.svg)](https://github.com/ChissaQAQ/TaskbarLyrics/actions/workflows/build.yml)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

在 Windows 任务栏里显示正在播放的歌的歌词。歌词条是任务栏的子窗口，不是浮在上面的悬浮窗。

![演示](docs/demo.gif)

## 下载

在 [Releases](https://github.com/ChissaQAQ/TaskbarLyrics/releases/latest) 下载 `TaskbarLyrics.exe`，双击运行，不用安装。

需要先装 [.NET 10 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/10.0)，没装的话程序会提示。

更新记录见 [CHANGELOG.md](CHANGELOG.md)。

### 前提：播放器必须打开「系统媒体控件（SMTC）」上报

默认时间轴方案通过 Windows 的系统媒体控件（SMTC）读取当前歌曲，需要播放器开启媒体信息上报。网易云桌面歌词 Hook 可直接取词，使用方法见下文。

检查方法：播放时按一下音量键，系统弹出的媒体浮层里能看到歌名和封面就没问题。

看不到的话去播放器设置里找，一般在「设置 → 播放」下面，名字类似「在系统媒体控件中显示」。网易云音乐和 QQ 音乐的部分版本默认是关的。Spotify 和浏览器默认会上报，不过浏览器在程序的黑名单里，要用的话在设置里取消屏蔽。

## 功能

- 任务栏模式和浮动窗口模式
- 宽度跟着歌词长短变，也可以自动避开任务栏图标
- 逐字高亮（酷狗 KRC），太长的行会跟着进度横向滚动
- 双行显示：外文歌下行是译文或罗马音，中文歌下行是下一句
- 第二行也可直接选择“下一句歌词”，有译文时仍显示下一句原文
- 鼠标移上去显示封面、上一首 / 播放 / 下一首和歌名
- 专辑封面
- 过滤作词、作曲这类制作信息
- 文字颜色跟随系统深浅色，也可以自定义
- 歌词条和设置窗口可分别选择 SkiaSharp 或原始 WPF 文字绘制，并独立调整 Hinting
- 居中或左对齐
- 全屏时自动隐藏
- 单曲循环时歌词会重新开始
- 播放器黑名单
- 自动检查更新，一键更新
- 歌词缓存到本地（30 天，最多 400 首），听过的歌断网也有词
- 锁定位置后鼠标穿透
- 听歌统计：累计听了多久、每天 / 周 / 月的时长、最常听的歌 / 歌手 / 专辑（按次数或时长排）、越听越多的歌手（跟上一段时间比）、一天里和一周里什么时候听，还有几张趣味小卡片；可以只看某个播放器
- 年度听歌报告，统计页和年报都能存成图片或复制去分享（图里没有文件路径）
- 统计页和年报的榜单配专辑封面、歌手头像
- 托盘图标

设置固定保存在 `%AppData%\TaskbarLyrics\config.json`，切换程序位置时共用这份配置。用户目录没有有效配置时，会自动迁移 exe 旁边的旧 `config.json`，并保留旧文件。要恢复默认设置，先退出程序，再删除用户目录和 exe 旁边的 `config.json`。

在“设置 → 外观”中，歌词条和设置窗口可分别选择 SkiaSharp（默认）或 WPF（原始）。歌词引擎点击“应用”后生效，歌名和歌手信息一起切换；设置窗口的引擎保存后，重新打开设置窗口生效。WPF 模式使用原生 TextBlock，SkiaSharp 模式使用 Skia 绘制字形。两种模式共用排版、字体回退和交互逻辑。输入框保留原生文字编辑，支持光标、选区和中文输入法。

歌词和窗口的字形对齐（Hinting）也分开保存，可选关闭、轻度、标准或完整，默认标准。对应引擎为 SkiaSharp 时可调整，WPF 模式下选项置灰并保留原值。旧版共用的 Hinting 会迁移到两边。关闭时保留字形轮廓，完整对齐更强调像素边界；实际差异取决于字体、字号和显示缩放。

听歌统计记在 `%AppData%\TaskbarLyrics\stats` 文件夹里，一个月一个文件，只存在本机，不上传。设置里可以关掉记录，也可以一键清空。每段记录有：开始时间和时区、听了多久、歌名、歌手、播放器，以及播放器报了的专辑、专辑歌手、曲目号、流派、随机 / 循环模式、歌曲时长和播放进度、这段为什么结束（暂停、切歌之类），还有歌词库里匹配到的歌曲 id 和专辑。

升级前留在 exe 旁边的听歌统计仍会读取，旧缓存保留在原处，新缓存写入用户目录。

统计页的封面和头像是播放时顺带存的：一首歌听满 8 秒后，封面优先用播放器给的那张，没有再从歌词的同一来源（网易云 / QQ 音乐）下载，歌手头像也从那里来。图存在 `config.json` 旁边的 `art-cache` 文件夹，打开统计页不联网，以前听的歌不会补图。设置里可以关掉，清空听歌记录时一起清掉。

## 原理

- 默认时间轴方案从 SMTC 读取歌名、歌手和播放状态。网易云不报播放进度时，通过本地计时推算
- 歌词依次从网易云、QQ 音乐、LRCLIB 找，逐字时间轴来自酷狗 KRC
- 用 `SetParent` 把窗口挂进任务栏（主屏 `Shell_TrayWnd`，副屏 `Shell_SecondaryTrayWnd`）

## 已知问题

- 默认时间轴方案在网易云不上报进度时无法感知拖动，程序启动前就在播放的歌也会从启动时开始计时
- 罗马音只有网易云有

## 网易云桌面歌词 Hook（实验性）

在“设置 → 歌词 → 歌词同步方案”中选择 Hook，并开启 64 位网易云客户端的桌面歌词。原有时间轴方案仍为默认选项，文字渲染引擎独立设置。

文字跟随缓存图像到最终窗口的绘制关系，预加载的下一句只有实际绘制后才显示。已建立关联的行不依赖字形相似度；尚未关联的缓存保留字形校验作为初始化兜底。

高亮从最终透明图像中读取。只有确认逐字信号后才跟随进度，没有逐字的歌曲整行显示。程序不合成匀速扫光，也不读取曲库时间戳推算 Hook 进度。

双行支持当前句和下一句上下交替，另一行作为第二行显示。连接状态只显示在设置页。暂时取不到文字时保留上一句，首次取词前保持空白。

歌词偏移和罗马音转换仅用于原时间轴方案。捕获的是绘制位置比例，精度受字间空白、字体和排版差异影响。关闭 Hook 会停止采集，DLL 在网易云退出时释放。

取词方式参考 [dsh-netease-lyrics](https://github.com/Hikari16665/dsh-netease-lyrics/blob/main/DEVELOPING.md)。原生模块使用 MinHook，许可见 [THIRD-PARTY.txt](native/LyricsHook/THIRD-PARTY.txt)。

## 开发

构建需要 .NET 10 SDK、CMake 和 Visual Studio 的“使用 C++ 的桌面开发”工具。首次构建会下载固定提交和校验值的 MinHook 源码。原生模块自动编译并嵌入单文件程序，启用 Hook 时解包到程序旁的 `.lyrics-hook` 缓存目录，目录不可写时使用 `%LocalAppData%\TaskbarLyrics\hooks`。

```powershell
dotnet build wpf/TaskbarLyrics -c Release

# 离线验证选句、渲染和设置，不连接播放器
dotnet run --project tests/TaskbarLyrics.Tests -c Release

# 先关掉正在运行的程序，不然 exe 被占用会报 MSB4018
Stop-Process -Name TaskbarLyrics -Force -ErrorAction SilentlyContinue
dotnet publish wpf/TaskbarLyrics -c Release -r win-x64 --self-contained false `
  -p:PublishSingleFile=true -o wpf/publish

# 测试歌词抓取（不读缓存）。时长秒可省略，网易云客户端不报时长
wpf/publish/TaskbarLyrics.exe --lyrics-test "歌名" "歌手" [translation|romaji|off] [时长秒]

# 测试检查更新
wpf/publish/TaskbarLyrics.exe --update-test

# 连接网易云桌面歌词，诊断 20 秒内的文字和高亮采样（不会修改同步方案设置）
# 先退出正在运行的任务栏歌词，避免两个读端控制同一通道
wpf/publish/TaskbarLyrics.exe --hook-test 20

# 正常运行并记录绘制关联日志；仅排查问题时启用
wpf/publish/TaskbarLyrics.exe --hook-trace

# 听歌统计自测：不带参数跑断言；report 对本机真实记录出文字报告；
# shot 把统计窗口、分享图、年度报告渲染成 PNG，浅色深色各一套（默认合成数据，加 real 用真实记录）
wpf/publish/TaskbarLyrics.exe --stats-test [report | shot <目录> [real]]
```

代码在 `wpf/TaskbarLyrics/`：

| 文件 | 内容 |
| --- | --- |
| `MainController.cs` | 主循环，读 SMTC、找当前行、交给界面 |
| `SmtcListener.cs` | 监听系统媒体控件 |
| `Lyrics.cs` | 抓歌词、选版本、对齐译文和逐字时间 |
| `DesktopLyricsHook.cs` | 读取可见歌词帧、选择当前句和实际高亮 |
| `LyricsCache.cs` | 歌词缓存 |
| `OverlayWindow.xaml(.cs)` | 歌词显示和动画 |
| `SkiaTextBlock.cs` | Skia 文字绘制、阴影和 DPI 位图缓存 |
| `NativeMethods.cs` | 挂进任务栏、置顶、鼠标穿透 |
| `TaskbarFreeSpace.cs` | 计算任务栏上的空位 |
| `SettingsWindow.xaml(.cs)` / `TrayIcon.cs` / `AppConfig.cs` | 设置、托盘、配置 |
| `Updater.cs` | 检查和安装更新 |
| `ListenStats.cs` | 记录听歌片段、读写统计文件 |
| `StatsReport.cs` / `StatsWindow.xaml(.cs)` | 汇总统计、统计窗口 |
| `ArtCollector.cs` / `ArtCache.cs` | 播放时收集封面和头像、本地图片缓存 |
| `YearReportWindow.xaml(.cs)` / `ShareImage.cs` | 年度报告、把界面存成分享图 |
| `StatsSelfTest.cs` | 听歌统计的控制台自测 |

不少看着多余的写法是为了绕开 Windows 的坑，注释里有写原因，改之前可以先看看。

原生代码在 `native/LyricsHook/`：`Hook.cpp` 读取最终像素，`DrawCapture.h` 捕获绘制入口，`DrawSources.h` 跟踪缓存与文字的关联。`Probe.cpp` 使用自建窗口验证捕获，不连接网易云。

### 发布

更新项目文件中的版本号和 `CHANGELOG.md`，提交并推送代码后，再推送同版本的标签，例如 `v1.0.32`。GitHub Actions 会校验标签与项目版本是否一致，验证成功后创建 Release，上传 `TaskbarLyrics.exe`，并从更新日志提取该版说明。

普通分支推送和 Pull Request 只验证、打包，不创建 Release。

## 反馈

有问题可以提 [Issue](https://github.com/ChissaQAQ/TaskbarLyrics/issues)。提之前先确认 SMTC 是通的，再附上程序、Windows 和播放器的版本。歌词有问题的话请写上歌名和歌手。

## 许可

代码使用 [MIT](LICENSE) 许可。

歌词、专辑封面和歌手头像来自网易云音乐、QQ 音乐、LRCLIB 和酷狗的公开接口，版权归原权利方。本地缓存只是为了少发请求：歌词 30 天后过期，图片超过 1500 张时先删最久没看的。仅供个人使用。
