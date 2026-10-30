// 绘制入口跟踪缓存关联；仅 --hook-trace 模式额外安装诊断探针并记录详细日志。
#pragma once

static void TraceLocked(const std::wstring& description, const RectF& destination = {}, const RectF& source = {}) {
    static unsigned count = 0;
    if ((channel->enabled & 2) && count < 20000) { ++count; Publish(description, destination, source, 0, 9); }
}

static void TraceDraw(const WCHAR* operation, const void* source, const void* destination,
    const RectF& destinationBounds = {}, const RectF& sourceBounds = {}) {
    if (inside || !channel || !(channel->enabled & 2) || !LyricsThread() || !TryAcquireSRWLockExclusive(&stateLock)) return;
    inside = true;
    WCHAR description[160];
    swprintf_s(description, L"%s source=%p destination=%p", operation, source, destination);
    TraceLocked(description, destinationBounds, sourceBounds);
    inside = false;
    ReleaseSRWLockExclusive(&stateLock);
}

static decltype(&GdipGetImageGraphicsContext) originalImageGraphics;
static GpStatus WINGDIPAPI CaptureImageGraphics(GpImage* image, GpGraphics** graphics) {
    const auto result = originalImageGraphics(image, graphics);
    if (result == Ok) { RecordImageGraphics(image, *graphics); TraceDraw(L"image.graphics", image, *graphics); }
    return result;
}

static decltype(&GdipCreateFromHDC) originalFromDC;
static GpStatus WINGDIPAPI CaptureFromDC(HDC dc, GpGraphics** graphics) {
    const auto result = originalFromDC(dc, graphics);
    if (result == Ok) {
        RecordDCGraphics(dc, *graphics);
        TraceDraw(L"dc.graphics", dc, *graphics);
        TraceDraw(L"bitmap.dc", GetCurrentObject(dc, OBJ_BITMAP), dc);
    }
    return result;
}

static decltype(&GdipCreateFromHDC2) originalFromDC2;
static GpStatus WINGDIPAPI CaptureFromDC2(HDC dc, HANDLE device, GpGraphics** graphics) {
    const auto result = originalFromDC2(dc, device, graphics);
    if (result == Ok) {
        RecordDCGraphics(dc, *graphics);
        TraceDraw(L"dc.graphics", dc, *graphics);
        TraceDraw(L"bitmap.dc", GetCurrentObject(dc, OBJ_BITMAP), dc);
    }
    return result;
}

static decltype(&GdipGetDC) originalGetDC;
static GpStatus WINGDIPAPI TraceGetDC(GpGraphics* graphics, HDC* dc) {
    const auto result = originalGetDC(graphics, dc);
    if (result == Ok) {
        TraceDraw(L"graphics.dc", graphics, *dc);
        TraceDraw(L"bitmap.dc", GetCurrentObject(*dc, OBJ_BITMAP), *dc);
    }
    return result;
}

static decltype(&GdipCreateHBITMAPFromBitmap) originalHbitmap;
static GpStatus WINGDIPAPI CaptureHbitmap(GpBitmap* bitmap, HBITMAP* output, ARGB background) {
    const auto result = originalHbitmap(bitmap, output, background);
    if (result == Ok) { RecordHbitmap(reinterpret_cast<GpImage*>(bitmap), *output); TraceDraw(L"image.hbitmap", bitmap, *output); }
    return result;
}

static decltype(&GdipBitmapLockBits) originalLockBits;
static GpStatus WINGDIPAPI TraceLockBits(GpBitmap* bitmap, const Rect* rectangle, UINT flags, PixelFormat format, BitmapData* data) {
    const auto result = originalLockBits(bitmap, rectangle, flags, format, data);
    if (result == Ok) TraceDraw(L"image.bits", bitmap, data->Scan0,
        RectF(0, 0, static_cast<REAL>(data->Width), static_cast<REAL>(data->Height)),
        RectF(static_cast<REAL>(data->Stride), static_cast<REAL>(flags), 0, 0));
    return result;
}

static decltype(&GdipDrawImage) originalDrawImage;
static GpStatus WINGDIPAPI TraceDrawImage(GpGraphics* graphics, GpImage* image, REAL x, REAL y) {
    const auto result = originalDrawImage(graphics, image, x, y);
    if (result == Ok) {
        RecordImageDraw(graphics, image, RectF(x, y, 0, 0), {}, true);
        TraceDraw(L"image.draw", image, graphics, RectF(x, y, 0, 0));
    }
    return result;
}

static decltype(&GdipDrawImageI) originalDrawImageI;
static GpStatus WINGDIPAPI TraceDrawImageI(GpGraphics* graphics, GpImage* image, INT x, INT y) {
    const auto result = originalDrawImageI(graphics, image, x, y);
    if (result == Ok) {
        const RectF output(static_cast<REAL>(x), static_cast<REAL>(y), 0, 0);
        RecordImageDraw(graphics, image, output, {}, true);
        TraceDraw(L"image.draw", image, graphics, output);
    }
    return result;
}

static decltype(&GdipDrawImageRect) originalDrawRect;
static GpStatus WINGDIPAPI TraceDrawRect(GpGraphics* graphics, GpImage* image, REAL x, REAL y, REAL width, REAL height) {
    const auto result = originalDrawRect(graphics, image, x, y, width, height);
    if (result == Ok) {
        RecordImageDraw(graphics, image, RectF(x, y, width, height), {}, true);
        TraceDraw(L"image.draw", image, graphics, RectF(x, y, width, height));
    }
    return result;
}

static decltype(&GdipDrawImageRectI) originalDrawRectI;
static GpStatus WINGDIPAPI TraceDrawRectI(GpGraphics* graphics, GpImage* image, INT x, INT y, INT width, INT height) {
    const auto result = originalDrawRectI(graphics, image, x, y, width, height);
    if (result == Ok) {
        const RectF output(static_cast<REAL>(x), static_cast<REAL>(y), static_cast<REAL>(width), static_cast<REAL>(height));
        RecordImageDraw(graphics, image, output, {}, true);
        TraceDraw(L"image.draw", image, graphics, output);
    }
    return result;
}

static decltype(&GdipDrawImageRectRect) originalDrawRectRect;
static GpStatus WINGDIPAPI CaptureDrawRectRect(GpGraphics* graphics, GpImage* image,
    REAL x, REAL y, REAL width, REAL height, REAL sx, REAL sy, REAL sw, REAL sh, GpUnit unit,
    const GpImageAttributes* attributes, DrawImageAbort callback, void* callbackData) {
    const auto result = originalDrawRectRect(graphics, image, x, y, width, height, sx, sy, sw, sh, unit, attributes, callback, callbackData);
    if (result == Ok) {
        RecordImageDraw(graphics, image, RectF(x, y, width, height), RectF(sx, sy, sw, sh));
        TraceDraw(L"image.draw", image, graphics, RectF(x, y, width, height), RectF(sx, sy, sw, sh));
    }
    return result;
}

static decltype(&GdipDrawImageRectRectI) originalDrawRectRectI;
static GpStatus WINGDIPAPI CaptureDrawRectRectI(GpGraphics* graphics, GpImage* image,
    INT x, INT y, INT width, INT height, INT sx, INT sy, INT sw, INT sh, GpUnit unit,
    const GpImageAttributes* attributes, DrawImageAbort callback, void* callbackData) {
    const auto result = originalDrawRectRectI(graphics, image, x, y, width, height, sx, sy, sw, sh, unit, attributes, callback, callbackData);
    if (result == Ok) {
        const RectF output(static_cast<REAL>(x), static_cast<REAL>(y), static_cast<REAL>(width), static_cast<REAL>(height));
        const RectF input(static_cast<REAL>(sx), static_cast<REAL>(sy), static_cast<REAL>(sw), static_cast<REAL>(sh));
        RecordImageDraw(graphics, image, output, input);
        TraceDraw(L"image.draw", image, graphics, output, input);
    }
    return result;
}

static decltype(&BitBlt) originalBitBlt;
static BOOL WINAPI CaptureBitBlt(HDC destination, int x, int y, int width, int height, HDC source, int sx, int sy, DWORD operation) {
    const auto result = originalBitBlt(destination, x, y, width, height, source, sx, sy, operation);
    if (result) {
        const RectF output(static_cast<REAL>(x), static_cast<REAL>(y), static_cast<REAL>(width), static_cast<REAL>(height));
        const RectF input(static_cast<REAL>(sx), static_cast<REAL>(sy), static_cast<REAL>(width), static_cast<REAL>(height));
        RecordBitmapDraw(source, destination, output, input);
        TraceDraw(L"bitmap.blit", GetCurrentObject(source, OBJ_BITMAP), GetCurrentObject(destination, OBJ_BITMAP), output, input);
    }
    return result;
}

static decltype(&StretchBlt) originalStretchBlt;
static BOOL WINAPI TraceStretchBlt(HDC destination, int x, int y, int width, int height,
    HDC source, int sx, int sy, int sw, int sh, DWORD operation) {
    const auto result = originalStretchBlt(destination, x, y, width, height, source, sx, sy, sw, sh, operation);
    if (result) TraceDraw(L"bitmap.stretch", GetCurrentObject(source, OBJ_BITMAP), GetCurrentObject(destination, OBJ_BITMAP),
        RectF(static_cast<REAL>(x), static_cast<REAL>(y), static_cast<REAL>(width), static_cast<REAL>(height)),
        RectF(static_cast<REAL>(sx), static_cast<REAL>(sy), static_cast<REAL>(sw), static_cast<REAL>(sh)));
    return result;
}

using AlphaBlendFunction = BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
static AlphaBlendFunction originalAlphaBlend;
static BOOL WINAPI TraceAlphaBlend(HDC destination, int x, int y, int width, int height,
    HDC source, int sx, int sy, int sw, int sh, BLENDFUNCTION blend) {
    const auto result = originalAlphaBlend(destination, x, y, width, height, source, sx, sy, sw, sh, blend);
    if (result) TraceDraw(L"bitmap.alpha", GetCurrentObject(source, OBJ_BITMAP), GetCurrentObject(destination, OBJ_BITMAP),
        RectF(static_cast<REAL>(x), static_cast<REAL>(y), static_cast<REAL>(width), static_cast<REAL>(height)),
        RectF(static_cast<REAL>(sx), static_cast<REAL>(sy), static_cast<REAL>(sw), static_cast<REAL>(sh)));
    return result;
}

static bool InstallDrawCapture(HMODULE gdiplus, bool trace) {
    bool complete = true;
    const auto install = [&](HMODULE module, const char* name, void* detour, void** original) {
        const auto status = MH_CreateHook(module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr, detour, original);
        if (status != MH_OK) {
            complete = false;
            WCHAR description[128];
            swprintf_s(description, L"trace.failed api=%S error=%d", name, status);
            TraceLocked(description);
        }
    };
#define TRACE_GDIP(name, detour, original) install(gdiplus, name, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original))
    TRACE_GDIP("GdipGetImageGraphicsContext", CaptureImageGraphics, originalImageGraphics);
    TRACE_GDIP("GdipCreateFromHDC", CaptureFromDC, originalFromDC);
    TRACE_GDIP("GdipCreateFromHDC2", CaptureFromDC2, originalFromDC2);
    TRACE_GDIP("GdipCreateHBITMAPFromBitmap", CaptureHbitmap, originalHbitmap);
    TRACE_GDIP("GdipDrawImageRectRect", CaptureDrawRectRect, originalDrawRectRect);
    TRACE_GDIP("GdipDrawImageRectRectI", CaptureDrawRectRectI, originalDrawRectRectI);
    TRACE_GDIP("GdipDeleteGraphics", CaptureDeleteGraphics, originalDeleteGraphics);
    TRACE_GDIP("GdipDisposeImage", CaptureDisposeImage, originalDisposeImage);
    TRACE_GDIP("GdipGraphicsClear", CaptureClearGraphics, originalClearGraphics);
    const auto gdi = GetModuleHandleW(L"gdi32.dll");
    install(gdi, "BitBlt", reinterpret_cast<void*>(CaptureBitBlt), reinterpret_cast<void**>(&originalBitBlt));
    install(gdi, "DeleteObject", reinterpret_cast<void*>(CaptureDeleteObject), reinterpret_cast<void**>(&originalDeleteObject));
    const bool sourcesReady = complete;
    if (trace) {
        TRACE_GDIP("GdipGetDC", TraceGetDC, originalGetDC);
        TRACE_GDIP("GdipBitmapLockBits", TraceLockBits, originalLockBits);
        TRACE_GDIP("GdipDrawImage", TraceDrawImage, originalDrawImage);
        TRACE_GDIP("GdipDrawImageI", TraceDrawImageI, originalDrawImageI);
        TRACE_GDIP("GdipDrawImageRect", TraceDrawRect, originalDrawRect);
        TRACE_GDIP("GdipDrawImageRectI", TraceDrawRectI, originalDrawRectI);
        install(gdi, "StretchBlt", reinterpret_cast<void*>(TraceStretchBlt), reinterpret_cast<void**>(&originalStretchBlt));
        install(LoadLibraryW(L"msimg32.dll"), "AlphaBlend", reinterpret_cast<void*>(TraceAlphaBlend), reinterpret_cast<void**>(&originalAlphaBlend));
        TraceLocked(complete ? L"trace.ready" : L"trace.incomplete");
    }
#undef TRACE_GDIP
    return sourcesReady;
}
