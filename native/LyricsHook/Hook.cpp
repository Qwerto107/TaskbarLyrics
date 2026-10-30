// 仅捕获 DesktopLyrics 所在线程的 GDI+ 文字路径；不改变播放器的绘制参数。
#include "Protocol.h"
#include <objidl.h>
#include <gdiplus.h>
#include <MinHook.h>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <vector>
#include <cmath>
#include <array>
using namespace Gdiplus;
using namespace Gdiplus::DllExports;

static decltype(&GdipAddPathString) originalText;
static decltype(&GdipFillPath) originalFill;
static decltype(&GdipDeletePath) originalDelete;
static decltype(&GdipClonePath) originalClone;
static decltype(&GdipResetPath) originalReset;
static decltype(&UpdateLayeredWindow) originalLayered;
static decltype(&UpdateLayeredWindowIndirect) originalLayeredIndirect;
static SRWLOCK stateLock = SRWLOCK_INIT;
static std::unordered_map<GpPath*, std::wstring> texts;
struct BitmapText { std::wstring text; RectF bounds; ARGB color; };
constexpr int MaskWidth = 128, MaskHeight = 24;
using GlyphMask = std::array<float, MaskWidth * MaskHeight>;
struct RecentText { BitmapText data; DWORD tick; GlyphMask mask; ARGB lastOpaqueColor; };
static std::vector<RecentText> recentTexts;
struct MatchedMask { uint64_t hash; std::wstring text; float width, height; };
static std::vector<MatchedMask> matchedMasks;
static Channel* channel;
static HANDLE signalEvent;
static thread_local bool inside;

static GlyphMask SmoothMask(const GlyphMask& input) {
    GlyphMask output{};
    for (int y = 0; y < MaskHeight; ++y) for (int x = 0; x < MaskWidth; ++x)
        for (int yy = std::max(0, y - 1); yy <= std::min(MaskHeight - 1, y + 1); ++yy)
            for (int xx = std::max(0, x - 1); xx <= std::min(MaskWidth - 1, x + 1); ++xx)
                output[y * MaskWidth + x] += input[yy * MaskWidth + xx];
    return output;
}

// 保留实际字形的归一化栅格，避免把等宽汉字组成的相邻句按长度误配。
static GlyphMask RasterMask(GpPath* path, const RectF& bounds) {
    GlyphMask mask{};
    const int width = static_cast<int>(std::ceil(bounds.Width)), height = static_cast<int>(std::ceil(bounds.Height));
    if (width <= 0 || width > 4096 || height <= 0 || height > 256) return mask;
    GpBitmap* bitmap = nullptr;
    GpGraphics* graphics = nullptr;
    GpSolidFill* brush = nullptr;
    if (GdipCreateBitmapFromScan0(width + 4, height + 4, 0, PixelFormat32bppPARGB, nullptr, &bitmap) != Ok) return mask;
    if (GdipGetImageGraphicsContext(reinterpret_cast<GpImage*>(bitmap), &graphics) == Ok &&
        GdipCreateSolidFill(0xffffffff, &brush) == Ok) {
        GdipSetSmoothingMode(graphics, SmoothingModeAntiAlias);
        GdipTranslateWorldTransform(graphics, 2 - bounds.X, 2 - bounds.Y, MatrixOrderAppend);
        originalFill(graphics, reinterpret_cast<GpBrush*>(brush), path);
        BitmapData data{};
        Rect rectangle(0, 0, width + 4, height + 4);
        if (GdipBitmapLockBits(bitmap, &rectangle, ImageLockModeRead, PixelFormat32bppPARGB, &data) == Ok) {
            int left = width + 4, top = height + 4, right = -1, bottom = -1;
            const auto ink = [&](int x, int y) {
                return (*reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(data.Scan0) + y * data.Stride + x * 4) >> 24) >= 40;
            };
            for (int y = 0; y < height + 4; ++y) for (int x = 0; x < width + 4; ++x) if (ink(x, y)) {
                left = std::min(left, x); right = std::max(right, x); top = std::min(top, y); bottom = std::max(bottom, y);
            }
            if (right >= left && bottom >= top)
                for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) if (ink(x, y))
                    mask[((y - top) * MaskHeight / (bottom - top + 1)) * MaskWidth + (x - left) * MaskWidth / (right - left + 1)] += 1;
            GdipBitmapUnlockBits(bitmap, &data);
        }
    }
    if (brush) GdipDeleteBrush(reinterpret_cast<GpBrush*>(brush));
    if (graphics) GdipDeleteGraphics(graphics);
    GdipDisposeImage(reinterpret_cast<GpImage*>(bitmap));
    return SmoothMask(mask);
}

static float MaskSimilarity(const GlyphMask& a, const GlyphMask& b) {
    float dot = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < a.size(); ++i) { dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return aa > 0 && bb > 0 ? dot / std::sqrt(aa * bb) : 0;
}

// 调用方持有 stateLock，单次写入整条采样，读端可以检测覆盖和不完整写入。
static void Publish(const std::wstring& text, const RectF& bounds, const RectF& clip, ARGB color, DWORD kind) {
    const LONG next = channel->latest + 1;
    auto& slot = channel->slots[(next - 1) % SlotCount];
    InterlockedExchange(&slot.guard, next * 2 - 1);
    slot.sequence = next;
    slot.tick = GetTickCount();
    slot.color = color;
    slot.kind = kind;
    slot.x = bounds.X; slot.y = bounds.Y;
    slot.width = bounds.Width; slot.height = bounds.Height;
    slot.clipX = clip.X; slot.clipY = clip.Y;
    slot.clipWidth = clip.Width; slot.clipHeight = clip.Height;
    wcsncpy_s(slot.text, text.c_str(), _TRUNCATE);
    InterlockedExchange(&slot.guard, next * 2);
    InterlockedExchange(&channel->latest, next);
    SetEvent(signalEvent);
}

static BOOL CALLBACK FindLyrics(HWND window, LPARAM value) {
    WCHAR name[64]{};
    GetClassNameW(window, name, 64);
    if (wcscmp(name, L"DesktopLyrics") == 0) {
        *reinterpret_cast<bool*>(value) = true;
        return FALSE;
    }
    return TRUE;
}

static bool LyricsThread() {
    thread_local DWORD checkedAt = 0;
    thread_local bool found = false;
    const DWORD now = GetTickCount();
    if (!checkedAt || now - checkedAt > 1000) {
        found = false;
        EnumThreadWindows(GetCurrentThreadId(), FindLyrics, reinterpret_cast<LPARAM>(&found));
        checkedAt = now;
    }
    return found;
}

#include "DrawSources.h"
#include "DrawCapture.h"

static GpStatus WINGDIPAPI CaptureText(GpPath* path, const WCHAR* value, INT length,
    const GpFontFamily* family, INT style, REAL size, const RectF* layout, const GpStringFormat* format) {
    const auto status = originalText(path, value, length, family, style, size, layout, format);
    if (status == Ok && !inside && channel && channel->enabled && value && LyricsThread()) {
        inside = true;
        const size_t count = length < 0 ? wcsnlen_s(value, TextCapacity - 1)
            : std::min(static_cast<size_t>(length), static_cast<size_t>(TextCapacity - 1));
        if (TryAcquireSRWLockExclusive(&stateLock)) {
            if (texts.size() > 256) texts.clear();
            auto& text = texts[path];
            if (text.size() + count < TextCapacity) text.append(value, count);
            RectF bounds{};
            if (GdipGetPathWorldBounds(path, &bounds, nullptr, nullptr) == Ok)
                Publish(text, bounds, RectF(0, 0, -1, 0), 0, 0); // -1 表示仅取到文字，尚无填充数据。
            if (channel->enabled & 2) {
                WCHAR familyName[LF_FACESIZE]{}, description[512];
                GdipGetFamilyName(family, familyName, 0);
                _snwprintf_s(description, _countof(description), _TRUNCATE, L"text path=%p font=%s size=%.2f style=%d value=%s", path, familyName, size, style, text.c_str());
                TraceLocked(description, bounds);
            }
            ReleaseSRWLockExclusive(&stateLock);
        }
        inside = false;
    }
    return status;
}

static GpStatus WINGDIPAPI CaptureFill(GpGraphics* graphics, GpBrush* brush, GpPath* path) {
    const auto result = originalFill(graphics, brush, path);
    if (result == Ok && !inside && channel && channel->enabled && TryAcquireSRWLockExclusive(&stateLock)) {
        inside = true;
        const auto found = texts.find(path);
        if (found != texts.end()) {
            RectF bounds{}, clip{};
            ARGB color = 0;
            // 纯色与渐变分别读取首色，仅作为绘制层标识，不把颜色本身当作高亮进度。
            if (GdipGetSolidFillColor(reinterpret_cast<GpSolidFill*>(brush), &color) != Ok) {
                ARGB colors[2]{};
                if (GdipGetLineColors(reinterpret_cast<GpLineGradient*>(brush), colors) == Ok) color = colors[0];
            }
            if (GdipGetPathWorldBounds(path, &bounds, nullptr, nullptr) == Ok &&
                GdipGetClipBounds(graphics, &clip) == Ok && bounds.Width > 0 && bounds.Height > 0) {
                Publish(found->second, bounds, clip, color, 1);
                RecordTextLocked(graphics, {found->second, bounds, color});
                if (channel->enabled & 2) {
                    WCHAR description[512];
                    _snwprintf_s(description, _countof(description), _TRUNCATE, L"fill graphics=%p path=%p color=%08X value=%s", graphics, path, color, found->second.c_str());
                    TraceLocked(description, bounds, clip);
                }
                auto recent = std::find_if(recentTexts.begin(), recentTexts.end(), [&](const RecentText& row) {
                    // 同一句预渲染到不同字号时，字形和尺寸必须属于同一个模板。
                    return row.data.text == found->second && std::abs(row.data.bounds.Width - bounds.Width) < 0.1f &&
                        std::abs(row.data.bounds.Height - bounds.Height) < 0.1f;
                });
                if (recent == recentTexts.end()) recentTexts.push_back({{found->second, bounds, color}, GetTickCount(), RasterMask(path, bounds), (color >> 24) >= 200 ? color : 0});
                else {
                    const int r = (color >> 16) & 255, g = (color >> 8) & 255, b = color & 255;
                    // 已唱和未唱图像会重复绘制同一句，灰色缓存不能覆盖已取得的高亮颜色。
                    if (std::max({r, g, b}) - std::min({r, g, b}) >= 20 && (color >> 24) >= 200)
                        recent->data.color = color;
                    recent->data.bounds = bounds;
                    recent->tick = GetTickCount();
                    if ((color >> 24) >= 200) recent->lastOpaqueColor = color;
                }
                if (recentTexts.size() > 16) recentTexts.erase(recentTexts.begin());

            }
        }
        inside = false;
        ReleaseSRWLockExclusive(&stateLock);
    }
    return result;
}

static GpStatus WINGDIPAPI CaptureDelete(GpPath* path) {
    AcquireSRWLockExclusive(&stateLock);
    texts.erase(path);
    ReleaseSRWLockExclusive(&stateLock);
    return originalDelete(path);
}

static GpStatus WINGDIPAPI CaptureReset(GpPath* path) {
    AcquireSRWLockExclusive(&stateLock);
    texts.erase(path);
    ReleaseSRWLockExclusive(&stateLock);
    return originalReset(path);
}

static GpStatus WINGDIPAPI CaptureClone(GpPath* path, GpPath** clone) {
    const auto status = originalClone(path, clone);
    if (status == Ok) {
        AcquireSRWLockExclusive(&stateLock);
        const auto found = texts.find(path);
        if (found != texts.end()) texts[*clone] = found->second;
        ReleaseSRWLockExclusive(&stateLock);
    }
    return status;
}

// 某些客户端在 CPU 中合成高亮，GDI/GDI+ 调用没有局部裁剪。
// 从最终提交的透明 DIB 读取实际着色边界，只有文字宽度匹配时才发布比例。
static void LayeredSample(HWND window, HDC source) {
    if (inside || !channel || !channel->enabled || !source) return;
    WCHAR name[64]{};
    GetClassNameW(window, name, 64);
    if (wcscmp(name, L"DesktopLyrics") != 0) return;
    static DWORD lastAt = 0;
    const DWORD now = GetTickCount();
    if (now - lastAt < 30 || !TryAcquireSRWLockExclusive(&stateLock)) return;
    inside = true;
    lastAt = now;
    DIBSECTION dib{};
    const auto bitmap = GetCurrentObject(source, OBJ_BITMAP);
    if (GetObjectW(bitmap, sizeof(dib), &dib) == sizeof(dib) && dib.dsBm.bmBits && dib.dsBm.bmBitsPixel == 32 &&
        dib.dsBm.bmWidth > 0 && dib.dsBm.bmWidth <= 4096 && dib.dsBm.bmHeight > 0 && dib.dsBm.bmHeight <= 512) {
        const int width = dib.dsBm.bmWidth, height = dib.dsBm.bmHeight;
        if (channel->enabled & 2) {
            WCHAR description[192];
            swprintf_s(description, L"frame dc=%p bitmap=%p bits=%p", source, bitmap, dib.dsBm.bmBits);
            TraceLocked(description, RectF(0, 0, static_cast<REAL>(width), static_cast<REAL>(height)));
        }
        struct Band { int top, bottom, left, right, activeRight, coloredPixels; };
        std::vector<Band> bands;
        // GetObject 返回的高度符号并不总能反映内存行方向，用源 DC 的像素核对。
        bool bottomUp = dib.dsBmih.biHeight > 0;
        bool orientationFound = false;
        int probes = 0;
        const auto rawPixel = [&](int x, int y) {
            return *reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(dib.dsBm.bmBits) + y * dib.dsBm.bmWidthBytes + x * 4);
        };
        const auto rgb = [](DWORD p) { return RGB((p >> 16) & 255, (p >> 8) & 255, p & 255); };
        for (int y = 0; y < height && !orientationFound && probes < 8; ++y)
            for (int x = 0; x < width && !orientationFound && probes < 8; ++x) {
                const DWORD direct = rawPixel(x, y), reverse = rawPixel(x, height - 1 - y);
                if (rgb(direct) == rgb(reverse)) continue;
                const COLORREF actual = GetPixel(source, x, y);
                ++probes;
                if (actual == rgb(direct)) { bottomUp = false; orientationFound = true; }
                else if (actual == rgb(reverse)) { bottomUp = true; orientationFound = true; }
            }
        const auto pixel = [&](int x, int y) {
            const int row = bottomUp ? height - 1 - y : y;
            return *reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(dib.dsBm.bmBits) + row * dib.dsBm.bmWidthBytes + x * 4);
        };
        // 仅诊断命令创建此通道；导出实际提交的图像，核对颜色和行位置。
        static DWORD snapshotAt = 0;
        if (now - snapshotAt > 1000) {
            snapshotAt = now;
            WCHAR snapshotName[160];
            ChannelName(snapshotName, 160, GetCurrentProcessId());
            wcscat_s(snapshotName, L".snapshot");
            const HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, snapshotName);
            if (mapping) {
                auto* snapshot = static_cast<LONG*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0));
                if (snapshot && InterlockedCompareExchange(snapshot, 3, 1) == 1) {
                    snapshot[1] = width; snapshot[2] = height;
                    auto* pixels = reinterpret_cast<DWORD*>(snapshot + 4);
                    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) pixels[y * width + x] = pixel(x, y);
                    InterlockedExchange(snapshot, 2);
                }
                if (snapshot) UnmapViewOfFile(snapshot);
                CloseHandle(mapping);
            }
        }
        const auto classify = [&](DWORD p) {
            const int alpha = p >> 24;
            if (alpha < 24) return 0;
            const int r = std::min(255, static_cast<int>((p >> 16) & 255) * 255 / alpha);
            const int g = std::min(255, static_cast<int>((p >> 8) & 255) * 255 / alpha);
            const int b = std::min(255, static_cast<int>(p & 255) * 255 / alpha);
            bool accent = false;
            for (const auto& text : recentTexts) {
                const DWORD c = text.data.color;
                if ((c >> 24) < 24 || now - text.tick > 30000) continue;
                const int cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
                const int low = std::min({r, g, b}), range = std::max({r, g, b}) - low;
                const int colorLow = std::min({cr, cg, cb}), colorRange = std::max({cr, cg, cb}) - colorLow;
                // 最终合成会改变亮度和饱和度；按色相匹配，灰白待唱层不参与。
                if (alpha < 224 || range < 20 || colorRange < 20) continue;
                if (std::abs(static_cast<float>(r - low) / range - static_cast<float>(cr - colorLow) / colorRange) < 0.18f &&
                    std::abs(static_cast<float>(g - low) / range - static_cast<float>(cg - colorLow) / colorRange) < 0.18f &&
                    std::abs(static_cast<float>(b - low) / range - static_cast<float>(cb - colorLow) / colorRange) < 0.18f) { accent = true; break; }
            }
            // 网易云也会把灰色字形缓存动态染色，绘制调用里没有对应颜色。
            // 此时读取最终图像的彩色字形；灰白文字和低透明度待唱层仍排除。
            accent = accent || (alpha >= 224 && std::max({r, g, b}) - std::min({r, g, b}) >= 30);
            // 待唱层也可能带颜色和透明度。它仍属于整句宽度，不能把它从分母中裁掉。
            const bool pending = std::max({(p >> 16) & 255, (p >> 8) & 255, p & 255}) > 15;
            return accent ? 2 : pending ? 1 : 0;
        };
        for (int y = 0; y < height; ++y) {
            int left = width, right = -1, active = -1, coloredPixels = 0;
            for (int x = 0; x < width; ++x) {
                const int kind = classify(pixel(x, y));
                if (kind) { left = std::min(left, x); right = x; }
                if (kind == 2) { active = x; ++coloredPixels; }
            }
            if (right < left) continue;
            if (bands.empty() || y - bands.back().bottom > 2) bands.push_back({y, y, left, right, active, coloredPixels});
            else {
                auto& band = bands.back();
                band.bottom = y; band.left = std::min(band.left, left);
                band.right = std::max(band.right, right); band.activeRight = std::max(band.activeRight, active);
                band.coloredPixels += coloredPixels;
            }
        }
        const auto surface = bitmapTexts.find(static_cast<HBITMAP>(bitmap));
        // 上下两行都已直接关联时，工具栏图标等像素区域不再进入歌词猜测。
        if (surface != bitmapTexts.end() && surface->second.size() >= 2) bands.clear();
        if (surface != bitmapTexts.end()) for (const auto& row : surface->second) {
            const int left = std::max(0, static_cast<int>(std::floor(row.bounds.X)) - 2);
            const int right = std::min(width - 1, static_cast<int>(std::ceil(row.bounds.GetRight())) + 2);
            const int top = std::max(0, static_cast<int>(std::floor(row.bounds.Y)) - 2);
            const int bottom = std::min(height - 1, static_cast<int>(std::ceil(row.bounds.GetBottom())) + 2);
            if (right < left || bottom < top) continue;
            // 行归属已由绘制调用确定。这里只读取该行的高亮，字形淡出或笔画断开也不丢文字。
            bands.erase(std::remove_if(bands.begin(), bands.end(), [&](const Band& band) {
                return band.bottom >= top && band.top <= bottom;
            }), bands.end());
            Band band{height, -1, width, -1, -1, 0};
            for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) {
                const int kind = classify(pixel(x, y));
                if (!kind) continue;
                band.top = std::min(band.top, y); band.bottom = std::max(band.bottom, y);
                band.left = std::min(band.left, x); band.right = std::max(band.right, x);
                if (kind == 2) { band.activeRight = std::max(band.activeRight, x); ++band.coloredPixels; }
            }
            if (band.right < band.left) band = {top, bottom, left, right, -1, 0};
            bands.push_back(band);
        }
        std::sort(bands.begin(), bands.end(), [](const Band& a, const Band& b) { return a.top < b.top; });
        // “二”“三”等单字的笔画可能被空白像素拆成多个带。只在合并后的
        // 宽高符合已捕获字形时拼回一行，避免把真正的上下两行按固定间距合并。
        for (size_t first = 0; first < bands.size(); ++first) {
            auto combined = bands[first], accepted = combined;
            size_t last = first;
            for (size_t next = first + 1; next < bands.size(); ++next) {
                const auto& part = bands[next];
                combined.bottom = part.bottom;
                combined.left = std::min(combined.left, part.left);
                combined.right = std::max(combined.right, part.right);
                combined.activeRight = std::max(combined.activeRight, part.activeRight);
                combined.coloredPixels += part.coloredPixels;
                const int w = combined.right - combined.left + 1, h = combined.bottom - combined.top + 1;
                const bool fits = std::any_of(recentTexts.begin(), recentTexts.end(), [&](const RecentText& text) {
                    return std::abs(text.data.bounds.Height - h) <= 3 &&
                        std::abs(text.data.bounds.Width - w) <= std::max(4.0f, w * 0.08f);
                });
                if (fits) { last = next; accepted = combined; }
            }
            if (last != first) {
                bands[first] = accepted;
                bands.erase(bands.begin() + first + 1, bands.begin() + last + 1);
            }
        }
        std::vector<std::wstring> used;
        int published = 0, directPublished = 0;
        int visibleBands = 0;
        std::wstring decisions;
        for (const auto& band : bands) {
            const int textWidth = band.right - band.left + 1, textHeight = band.bottom - band.top + 1;
            // 单字也可能只有一横或很窄，不能按行的最小宽高排除。
            if (textHeight * textWidth < 4) continue;
            ++visibleBands;
            const auto* direct = DrawnText(static_cast<HBITMAP>(bitmap),
                RectF(static_cast<REAL>(band.left), static_cast<REAL>(band.top), static_cast<REAL>(textWidth), static_cast<REAL>(textHeight)));
            const RecentText* chosen = nullptr;
            float best = 0, second = 0;
            if (!direct) {
                GlyphMask target{};
                for (int y = band.top; y <= band.bottom; ++y) for (int x = band.left; x <= band.right; ++x) {
                    const DWORD p = pixel(x, y);
                    if ((p >> 24) >= 24 && std::max({(p >> 16) & 255, (p >> 8) & 255, p & 255}) > 15)
                        target[((y - band.top) * MaskHeight / textHeight) * MaskWidth + (x - band.left) * MaskWidth / textWidth] += 1;
                }
                // 高亮颜色变化通常不改变字形。精确指纹相同才复用，避免同长度歌词串行。
                uint64_t hash = 1469598103934665603ULL;
                for (float value : target) { hash ^= static_cast<uint64_t>(value); hash *= 1099511628211ULL; }
                const auto cached = std::find_if(matchedMasks.begin(), matchedMasks.end(), [&](const MatchedMask& item) { return item.hash == hash; });
                if (cached != matchedMasks.end()) {
                    const auto found = std::find_if(recentTexts.begin(), recentTexts.end(), [&](const RecentText& item) {
                        return item.data.text == cached->text && std::abs(item.data.bounds.Width - cached->width) < 0.1f &&
                            std::abs(item.data.bounds.Height - cached->height) < 0.1f;
                    });
                    if (found != recentTexts.end()) { chosen = &*found; best = 1; }
                }
                if (!chosen) target = SmoothMask(target);
                for (auto it = recentTexts.rbegin(); it != recentTexts.rend(); ++it) {
                    if (chosen && best == 1) break;
                    if (std::find(used.begin(), used.end(), it->data.text) != used.end()) continue;
                    const float scale = textHeight / it->data.bounds.Height;
                    if (scale < 0.5f || scale > 4) continue;
                    const float raw = std::abs(it->data.bounds.Width - textWidth);
                    const float scaled = std::abs(it->data.bounds.Width * scale - textWidth) + 8;
                    const float score = std::min(raw, scaled);
                    if (score > std::max(6.0f, textWidth * 0.08f)) continue;
                    const float similarity = MaskSimilarity(it->mask, target);
                    // 同一文本的不同字号不是竞争歌词，不应因此触发歧义拒绝。
                    if (chosen && it->data.text == chosen->data.text) {
                        if (similarity > best) { best = similarity; chosen = &*it; }
                    }
                    else if (similarity > best) { second = best; best = similarity; chosen = &*it; }
                    else second = std::max(second, similarity);
                }
                WCHAR decision[96];
                swprintf_s(decision, L"match=%.3f/%.3f size=%dx%d; ", best, second, textWidth, textHeight);
                if (decisions.size() < 200) decisions += decision;
                if (!chosen || best < 0.78f || best - second < 0.025f) continue;
                if (cached == matchedMasks.end()) {
                    if (matchedMasks.size() >= 16) matchedMasks.erase(matchedMasks.begin());
                    matchedMasks.push_back({hash, chosen->data.text, chosen->data.bounds.Width, chosen->data.bounds.Height});
                }
            } else {
                ++directPublished;
                if (decisions.size() < 200) decisions += L"direct; ";
            }
            const auto& selected = direct ? *direct : chosen->data;
            used.push_back(selected.text);
            int activeRight = band.activeRight;
            DWORD opaque = chosen ? chosen->lastOpaqueColor : 0;
            if (direct) for (auto it = recentTexts.rbegin(); it != recentTexts.rend(); ++it)
                if (it->data.text == direct->text) { opaque = it->lastOpaqueColor; break; }
            // 白色前景只在实际绘制调用确认时启用，不能把任意白色待唱层当作高亮。
            if (band.coloredPixels < 16 && (opaque >> 24) >= 200 &&
                ((opaque >> 16) & 255) >= 230 && ((opaque >> 8) & 255) >= 230 && (opaque & 255) >= 230) {
                activeRight = -1;
                for (int y = band.top; y <= band.bottom; ++y) for (int x = band.left; x <= band.right; ++x) {
                    const DWORD p = pixel(x, y); const int alpha = p >> 24;
                    if (alpha < 224) continue;
                    const int r = std::min(255, static_cast<int>((p >> 16) & 255) * 255 / alpha);
                    const int g = std::min(255, static_cast<int>((p >> 8) & 255) * 255 / alpha);
                    const int b = std::min(255, static_cast<int>(p & 255) * 255 / alpha);
                    if (std::min({r, g, b}) >= 230 && std::max({r, g, b}) - std::min({r, g, b}) < 12)
                        activeRight = std::max(activeRight, x);
                }
            }
            float progressWidth = static_cast<float>(std::max(0, activeRight - band.left + 1));
            // 抗锯齿末端可能只剩 1~2 像素，已唱完的上一句不能一直被当作活动行。
            if (progressWidth > 0 && progressWidth >= textWidth - 2) progressWidth = static_cast<float>(textWidth);
            Publish(selected.text, RectF(static_cast<float>(band.left), static_cast<float>(band.top),
                static_cast<float>(textWidth), static_cast<float>(textHeight)),
                RectF(static_cast<float>(band.left), static_cast<float>(band.top), progressWidth, static_cast<float>(textHeight)),
                selected.color, 4);
            ++published;
        }
        // 一帧的行集合完整提交，读端不把上一帧下行拼到本帧上行。
        Publish(L"", RectF(static_cast<REAL>(directPublished), 0, 0, 0), RectF(),
            static_cast<ARGB>(published | (std::min(2, visibleBands) << 16)), 8);
        submittedBitmaps.insert(static_cast<HBITMAP>(bitmap));
        static DWORD debugAt = 0;
        if (now - debugAt > 1000) {
            debugAt = now;
            std::wstring description = decisions;
            WCHAR part[128];
            for (const auto& band : bands) {
                swprintf_s(part, L"band=(%d,%d,%d,%d) active=%d; ", band.left, band.top, band.right, band.bottom, band.activeRight);
                description += part;
                if (description.size() > 200) break;
            }
            for (auto it = recentTexts.rbegin(); it != recentTexts.rend() && description.size() < 400; ++it) {
                swprintf_s(part, L"path=(%.1f,%.1f) color=%08X; ", it->data.bounds.Width, it->data.bounds.Height, it->data.color);
                description += part;
            }
            Publish(description, RectF(), RectF(), 0, 7);
        }
    }
    inside = false;
    ReleaseSRWLockExclusive(&stateLock);
}

static BOOL WINAPI CaptureLayered(HWND window, HDC destination, POINT* position, SIZE* size, HDC source,
    POINT* origin, COLORREF key, BLENDFUNCTION* blend, DWORD flags) {
    const BOOL result = originalLayered(window, destination, position, size, source, origin, key, blend, flags);
    if (result) LayeredSample(window, source);
    return result;
}

static BOOL WINAPI CaptureLayeredIndirect(HWND window, const UPDATELAYEREDWINDOWINFO* info) {
    const BOOL result = originalLayeredIndirect(window, info);
    if (result && info) LayeredSample(window, info->hdcSrc);
    return result;
}

static DWORD WINAPI Initialize(void*) {
    WCHAR name[128];
    ChannelName(name, 128, GetCurrentProcessId());
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Channel), name);
    if (!mapping) return 1;
    channel = static_cast<Channel*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Channel)));
    if (!channel) { CloseHandle(mapping); return 2; }
    wcscat_s(name, L".event");
    signalEvent = CreateEventW(nullptr, FALSE, FALSE, name);
    if (!signalEvent) return 3;
    channel->version = ProtocolVersion;
    const HMODULE gdiplus = GetModuleHandleW(L"gdiplus.dll");
    if (!gdiplus || MH_Initialize() != MH_OK) return 4;
    const auto hook = [gdiplus](const char* name, void* detour, void** original) {
        return MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(gdiplus, name)), detour, original) == MH_OK;
    };
    if (!hook("GdipAddPathString", reinterpret_cast<void*>(CaptureText), reinterpret_cast<void**>(&originalText)) ||
        !hook("GdipFillPath", reinterpret_cast<void*>(CaptureFill), reinterpret_cast<void**>(&originalFill)) ||
        !hook("GdipDeletePath", reinterpret_cast<void*>(CaptureDelete), reinterpret_cast<void**>(&originalDelete)) ||
        !hook("GdipResetPath", reinterpret_cast<void*>(CaptureReset), reinterpret_cast<void**>(&originalReset)) ||
        !hook("GdipClonePath", reinterpret_cast<void*>(CaptureClone), reinterpret_cast<void**>(&originalClone))) return 5;
    const auto user = GetModuleHandleW(L"user32.dll");
    if (MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(user, "UpdateLayeredWindow")), reinterpret_cast<void*>(CaptureLayered),
        reinterpret_cast<void**>(&originalLayered)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(GetProcAddress(user, "UpdateLayeredWindowIndirect")), reinterpret_cast<void*>(CaptureLayeredIndirect),
        reinterpret_cast<void**>(&originalLayeredIndirect)) != MH_OK) return 11;
    if (!InstallDrawCapture(gdiplus, (channel->enabled & 2) != 0)) return 12;
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return 6;
    InterlockedExchange(&channel->ready, 1);
    SetEvent(signalEvent);
    return 0;
}

static DWORD WINAPI Start(void* argument) {
    const DWORD result = Initialize(argument);
    if (result && channel) {
        InterlockedExchange(&channel->ready, -static_cast<LONG>(result));
        if (signalEvent) SetEvent(signalEvent);
    }
    return result;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // 初始化离开 loader lock 执行；DLL 保留到播放器退出，避免回调执行中卸载。
        const HANDLE thread = CreateThread(nullptr, 0, Start, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
