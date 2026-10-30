// 离线验证：在自建 DesktopLyrics 窗口线程绘制文字及裁剪高亮，不访问网易云。
#include "Protocol.h"
#include <objidl.h>
#include <gdiplus.h>
#include <cstdio>
#include <initializer_list>
#include <cmath>
#include <algorithm>
using namespace Gdiplus;
int wmain(int argc, WCHAR** argv) {
    if (argc < 2 || argc > 3) return 1;
    const bool trace = argc == 3 && wcscmp(argv[2], L"--trace") == 0;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DesktopLyrics";
    RegisterClassW(&wc);
    const HWND window = CreateWindowExW(WS_EX_LAYERED, wc.lpszClassName, L"Offline probe", WS_POPUP, 0, 0, 600, 100,
        nullptr, nullptr, wc.hInstance, nullptr);
    ULONG_PTR token;
    GdiplusStartupInput input;
    if (GdiplusStartup(&token, &input, nullptr) != Ok) return 2;
    WCHAR name[128];
    ChannelName(name, 128, GetCurrentProcessId());
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Channel), name);
    auto* channel = mapping ? static_cast<Channel*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Channel))) : nullptr;
    if (!channel) return 3;
    channel->enabled = trace ? 3 : 1;
    if (!LoadLibraryW(argv[1])) return 2;
    for (int i = 0; i < 100; ++i) {
        if (channel && channel->ready) break;
        Sleep(30);
    }
    if (!channel || !channel->ready) return 3;
    {
        Bitmap pending(600, 100, PixelFormat32bppARGB), accent(600, 100, PixelFormat32bppARGB);
        Graphics pendingGraphics(&pending), accentGraphics(&accent);
        FontFamily preferred(L"Microsoft YaHei UI");
        const auto* family = preferred.GetLastStatus() == Ok ? &preferred : FontFamily::GenericSansSerif();
        GraphicsPath path;
        path.AddString(L"位图高亮测试", -1, family, FontStyleRegular, 24, PointF(10, 10), nullptr);
        // 白色待唱层会触发旧的泛白色识别错误，必须保持进度为零。
        SolidBrush dim(Color(255, 255, 255, 255)), bright(Color(255, 195, 195, 195));
        pendingGraphics.FillPath(&dim, &path);
        accentGraphics.FillPath(&bright, &path);
        GraphicsPath nextPath;
        nextPath.AddString(L"测试不同内容", -1, family, FontStyleRegular, 24, PointF(10, 55), nullptr);
        pendingGraphics.FillPath(&dim, &nextPath);
        accentGraphics.FillPath(&bright, &nextPath);
        HBITMAP dimBitmap, brightBitmap;
        pending.GetHBITMAP(Color(0), &dimBitmap);
        accent.GetHBITMAP(Color(0), &brightBitmap);
        // 再缓存一条等字数歌词，确认最终文字不是仅靠宽度选出来的。
        Bitmap decoy(600, 100, PixelFormat32bppARGB);
        Graphics decoyGraphics(&decoy);
        GraphicsPath decoyPath;
        decoyPath.AddString(L"测试不同内容", -1, family, FontStyleRegular, 24, PointF(10, 10), nullptr);
        decoyGraphics.FillPath(&bright, &decoyPath);
        const HDC screen = GetDC(window);
        const HDC source = CreateCompatibleDC(screen), target = CreateCompatibleDC(screen);
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = 600; bitmapInfo.bmiHeader.biHeight = -100;
        bitmapInfo.bmiHeader.biPlanes = 1; bitmapInfo.bmiHeader.biBitCount = 32;
        void* bits;
        const HBITMAP output = CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
        const auto adjustBrightness = [&] {
            auto* pixels = static_cast<DWORD*>(bits);
            for (int i = 0; i < 600 * 100; ++i) {
                const DWORD p = pixels[i];
                const DWORD a = p >> 24;
                // 模拟网易云把灰色字形缓存动态染成粉色，原始填充调用没有粉色。
                if (a > 0 && ((p >> 16) & 255) * 255 / a < 220)
                    pixels[i] = a << 24 | (229 * a / 255) << 16 | (150 * a / 255) << 8 | (150 * a / 255);
            }
        };
        const auto oldTarget = SelectObject(target, output);
        const auto oldSource = SelectObject(source, dimBitmap);
        BitBlt(target, 0, 0, 600, 100, source, 0, 0, SRCCOPY);
        SelectObject(source, brightBitmap);
        BitBlt(target, 0, 0, 60, 45, source, 0, 0, SRCCOPY);
        adjustBrightness();
        POINT position{}, origin{};
        SIZE size{600, 100};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window, screen, &position, &size, target, &origin, 0, &blend, ULW_ALPHA)) return 7;
        bool foundBoundary = false;
        bool pendingIsZero = false;
        for (LONG i = 0; i < channel->latest && i < SlotCount; ++i) {
            const auto& boundary = channel->slots[i];
            if (boundary.kind == 4 && wcscmp(boundary.text, L"位图高亮测试") == 0)
                foundBoundary |= boundary.clipWidth > 0 && boundary.clipWidth < boundary.width && boundary.y < 45;
            if (boundary.kind == 4 && wcscmp(boundary.text, L"测试不同内容") == 0)
                pendingIsZero |= boundary.clipWidth == 0 && boundary.y > 45;
        }
        if (!pendingIsZero) return 9;
        const LONG previousFrame = channel->latest;
        SelectObject(source, dimBitmap);
        BitBlt(target, 0, 0, 600, 100, source, 0, 0, SRCCOPY);
        SelectObject(source, brightBitmap);
        BitBlt(target, 0, 0, 600, 45, source, 0, 0, SRCCOPY);
        BitBlt(target, 0, 45, 60, 55, source, 0, 45, SRCCOPY);
        adjustBrightness();
        Sleep(35);
        if (!UpdateLayeredWindow(window, screen, &position, &size, target, &origin, 0, &blend, ULW_ALPHA)) return 10;
        bool topComplete = false, bottomActive = false;
        for (LONG i = previousFrame; i < channel->latest && i < SlotCount; ++i) {
            const auto& sample = channel->slots[i];
            if (sample.kind != 4) continue;
            if (sample.y < 45) topComplete |= sample.clipWidth == sample.width;
            else bottomActive |= sample.clipWidth > 0 && sample.clipWidth < sample.width;
        }
        if (!topComplete || !bottomActive) return 11;
        // 白色已唱/灰色待唱，仅实际白色前景允许启用白色进度。
        SolidBrush gray(Color(255, 90, 90, 90));
        pendingGraphics.Clear(Color(0));
        accentGraphics.Clear(Color(0));
        pendingGraphics.FillPath(&gray, &path);
        accentGraphics.FillPath(&dim, &path);
        HBITMAP grayBitmap, whiteBitmap;
        pending.GetHBITMAP(Color(0), &grayBitmap);
        accent.GetHBITMAP(Color(0), &whiteBitmap);
        SelectObject(source, grayBitmap);
        BitBlt(target, 0, 0, 600, 100, source, 0, 0, SRCCOPY);
        SelectObject(source, whiteBitmap);
        BitBlt(target, 0, 0, 60, 45, source, 0, 0, SRCCOPY);
        const LONG whiteStart = channel->latest;
        Sleep(35);
        if (!UpdateLayeredWindow(window, screen, &position, &size, target, &origin, 0, &blend, ULW_ALPHA)) return 12;
        bool whiteProgress = false;
        for (LONG i = whiteStart; i < channel->latest && i < SlotCount; ++i) {
            const auto& sample = channel->slots[i];
            whiteProgress |= sample.kind == 4 && sample.y < 45 && sample.clipWidth > 0 && sample.clipWidth < sample.width;
        }
        if (!whiteProgress) return 13;
        if (!foundBoundary) {
            for (LONG i = 0; i < channel->latest && i < SlotCount; ++i)
                if (channel->slots[i].kind == 7) wprintf(L"%s\n", channel->slots[i].text);
            return 8;
        }
        SelectObject(source, oldSource); SelectObject(target, oldTarget);
        DeleteObject(grayBitmap); DeleteObject(whiteBitmap);
        DeleteObject(dimBitmap); DeleteObject(brightBitmap); DeleteObject(output);
        DeleteDC(source); DeleteDC(target); ReleaseDC(window, screen);
    }
    if (trace) {
        bool graphics = false, bitmap = false, copy = false, frame = false;
        for (LONG sequence = std::max<LONG>(1, channel->latest - SlotCount + 1); sequence <= channel->latest; ++sequence) {
            const auto& sample = channel->slots[(sequence - 1) % SlotCount];
            if (sample.kind != 9) continue;
            graphics |= wcsstr(sample.text, L"image.graphics") != nullptr;
            bitmap |= wcsstr(sample.text, L"image.hbitmap") != nullptr;
            copy |= wcsstr(sample.text, L"bitmap.blit") != nullptr;
            frame |= wcsstr(sample.text, L"frame dc=") != nullptr;
        }
        if (!graphics || !bitmap || !copy || !frame) return 18;
        puts("PASS: diagnostic image/graphics/bitmap/copy/frame links");
    }
    InterlockedExchange(&channel->latest, 0);
    {
        // 短句不能依赖字数；模拟低透明度待唱、描边和同一句在两行使用不同字号。
        const HDC screen = GetDC(window), target = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = 600; info.bmiHeader.biHeight = -100;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* bits;
        const HBITMAP output = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        const auto previousBitmap = SelectObject(target, output);
        FontFamily preferred(L"Microsoft YaHei UI");
        const auto* family = preferred.GetLastStatus() == Ok ? &preferred : FontFamily::GenericSansSerif();
        SolidBrush faint(Color(51, 153, 153, 153)), active(Color(255, 229, 150, 150));
        Pen outline(Color(51, 153, 153, 153), 1.5f);
        bool allMatched = true;
        for (const auto* text : {L"啊", L"喔", L"一", L"二", L"三", L"心", L"了", L"I", L"喔喔", L"别分别", L"为你画一幅", L"贝斯: 王宝新"}) {
            Bitmap bitmap(600, 100, PixelFormat32bppPARGB);
            Graphics graphics(&bitmap);
            graphics.SetSmoothingMode(SmoothingModeAntiAlias);
            graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
            GraphicsPath largePath, shortPath, mainPath;
            largePath.AddString(text, -1, family, FontStyleRegular, 28, PointF(10, 10), nullptr);
            graphics.FillPath(&active, &largePath);
            graphics.Clear(Color(0));
            mainPath.AddString(L"窒息到快要不能呼吸", -1, family, FontStyleRegular, 24, PointF(80.3f, 10.4f), nullptr);
            shortPath.AddString(text, -1, family, FontStyleRegular, 20, PointF(240.2f, 56.3f), nullptr);
            graphics.FillPath(&active, &mainPath);
            graphics.DrawPath(&outline, &shortPath);
            graphics.FillPath(&faint, &shortPath);
            Bitmap future(600, 100, PixelFormat32bppPARGB);
            Graphics futureGraphics(&future);
            GraphicsPath futurePath;
            futurePath.AddString(L"不该提前跑", -1, family, FontStyleRegular, 20, PointF(240.2f, 56.3f), nullptr);
            futureGraphics.FillPath(&active, &futurePath); // 已捕获但未复制到最终窗口，不能显示。
            // 和真机日志一致：缓存图像通过 GDI+ 绘制进最终 DC，不能用 memcpy 绕过关联链。
            {
                Graphics outputGraphics(target);
                outputGraphics.SetCompositingMode(CompositingModeSourceCopy);
                outputGraphics.Clear(Color(0));
                outputGraphics.DrawImage(&bitmap, Rect(0, 0, 600, 100), 0, 0, 600, 100, UnitPixel);
            }
            const LONG before = channel->latest;
            POINT position{}, origin{}; SIZE size{600, 100};
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            Sleep(40);
            if (!UpdateLayeredWindow(window, screen, &position, &size, target, &origin, 0, &blend, ULW_ALPHA)) return 15;
            bool shortMatched = false, mainMatched = false, directDouble = false;
            for (LONG sequence = before + 1; sequence <= channel->latest; ++sequence) {
                const auto& sample = channel->slots[(sequence - 1) % SlotCount];
                directDouble |= sample.kind == 8 && sample.x == 2;
                if (sample.kind != 4) continue;
                shortMatched |= wcscmp(sample.text, text) == 0 && sample.y > 50 && sample.clipWidth == 0;
                mainMatched |= wcscmp(sample.text, L"窒息到快要不能呼吸") == 0 && sample.y < 50;
            }
            // 双行切为单行后，只应保留真正可见的短句。
            {
                Graphics outputGraphics(target);
                outputGraphics.SetCompositingMode(CompositingModeSourceCopy);
                outputGraphics.Clear(Color(0));
                outputGraphics.DrawImage(&bitmap, Rect(0, 50, 600, 50), 0, 50, 600, 50, UnitPixel);
            }
            const LONG singleStart = channel->latest;
            Sleep(40);
            if (!UpdateLayeredWindow(window, screen, &position, &size, target, &origin, 0, &blend, ULW_ALPHA)) return 17;
            bool singleMatched = false, singleFrame = false;
            for (LONG sequence = singleStart + 1; sequence <= channel->latest; ++sequence) {
                const auto& sample = channel->slots[(sequence - 1) % SlotCount];
                singleMatched |= sample.kind == 4 && wcscmp(sample.text, text) == 0 && sample.clipWidth == 0;
                singleFrame |= sample.kind == 8 && sample.color == (1 | (1 << 16)) && sample.x == 1;
            }
            const bool matched = shortMatched && mainMatched && directDouble && singleMatched && singleFrame;
            if (!matched) printf("  details short=%d main=%d directDouble=%d single=%d singleFrame=%d\n",
                shortMatched, mainMatched, directDouble, singleMatched, singleFrame);
            wprintf(L"short line %s: %s\n", text, matched ? L"PASS" : L"FAIL");
            allMatched &= matched;
        }
        SelectObject(target, previousBitmap); DeleteObject(output);
        DeleteDC(target); ReleaseDC(window, screen);
        if (!allMatched) return 16;
    }
    channel->enabled = 1;
    InterlockedExchange(&channel->latest, 0);
    {
        Bitmap bitmap(600, 100, PixelFormat32bppARGB);
        Graphics graphics(&bitmap);
        FontFamily preferred(L"Microsoft YaHei UI");
        const auto* family = preferred.GetLastStatus() == Ok ? &preferred : FontFamily::GenericSansSerif();
        GraphicsPath path;
        path.AddString(L"捕获歌词进度", -1, family, FontStyleRegular, 24, PointF(10, 10), nullptr);
        RectF bounds;
        path.GetBounds(&bounds);
        SolidBrush pending(Color(255, 100, 100, 100)), accent(Color(255, 255, 220, 0));
        for (float fraction : {0.25f, 0.5f, 0.75f}) {
            graphics.ResetClip();
            graphics.FillPath(&pending, &path);
            graphics.SetClip(RectF(bounds.X - 1, bounds.Y - 1, bounds.Width * fraction + 1, bounds.Height + 2));
            graphics.FillPath(&accent, &path);
        }
        if (channel->latest != 7) return 4;
        for (int i = 2; i < 7; i += 2) {
            const auto& sample = channel->slots[i];
            const float fraction = (sample.clipX + sample.clipWidth - sample.x) / sample.width;
            if (wcscmp(sample.text, L"捕获歌词进度") || std::abs(fraction - i / 8.0f) > 0.01f) return 5;
        }
    }
    channel->enabled = 0;
    UnmapViewOfFile(channel);
    CloseHandle(mapping);
    DestroyWindow(window);
    GdiplusShutdown(token);
    puts("PASS: text, clipped fills, and actual layered-window pixel boundary");
    return 0;
}
