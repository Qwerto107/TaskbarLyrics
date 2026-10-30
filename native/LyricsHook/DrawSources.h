// 把已捕获文字跟随图像复制到最终位图；预加载缓存只有实际绘制后才成为可见歌词。
#pragma once
#include <unordered_set>

struct GraphicsSource { GpImage* image; HBITMAP bitmap; };
static std::unordered_map<GpGraphics*, GraphicsSource> graphicsSources;
static std::unordered_map<GpImage*, std::vector<BitmapText>> imageTexts;
static std::unordered_map<HBITMAP, std::vector<BitmapText>> bitmapTexts;
static std::unordered_set<HBITMAP> submittedBitmaps;

struct SourceLock {
    bool held;
    explicit SourceLock(bool cleanup = false) : held(!inside && channel && (cleanup || (channel->enabled && LyricsThread())) &&
        TryAcquireSRWLockExclusive(&stateLock)) { if (held) inside = true; }
    ~SourceLock() { if (held) { inside = false; ReleaseSRWLockExclusive(&stateLock); } }
    explicit operator bool() const { return held; }
};

static std::vector<BitmapText>* GraphicsTexts(GpGraphics* graphics) {
    const auto owner = graphicsSources.find(graphics);
    if (owner == graphicsSources.end()) return nullptr;
    if (owner->second.image) return &imageTexts[owner->second.image];
    if (!owner->second.bitmap) return nullptr;
    auto& rows = bitmapTexts[owner->second.bitmap];
    // 最终窗口每帧重新收集实际绘制的行，预加载/上一帧的第二行不混入本帧。
    if (submittedBitmaps.erase(owner->second.bitmap)) rows.clear();
    return &rows;
}

static void PutText(std::vector<BitmapText>& rows, const BitmapText& text) {
    const float center = text.bounds.Y + text.bounds.Height / 2;
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const BitmapText& previous) {
        return std::abs(previous.bounds.Y + previous.bounds.Height / 2 - center) <= (previous.bounds.Height + text.bounds.Height) / 2 + 2;
    }), rows.end());
    if (rows.size() < 8) rows.push_back(text);
}

static void RecordImageGraphics(GpImage* image, GpGraphics* graphics) {
    SourceLock lock;
    if (!lock) return;
    if (graphicsSources.size() > 1024) graphicsSources.clear();
    graphicsSources[graphics] = {image, nullptr};
}

static void RecordDCGraphics(HDC dc, GpGraphics* graphics) {
    SourceLock lock;
    if (lock) graphicsSources[graphics] = {nullptr, static_cast<HBITMAP>(GetCurrentObject(dc, OBJ_BITMAP))};
}

static void RecordTextLocked(GpGraphics* graphics, const BitmapText& text) {
    if (auto* rows = GraphicsTexts(graphics)) PutText(*rows, text);
}

static void CopyTexts(const std::vector<BitmapText>& source, std::vector<BitmapText>& destination,
    const RectF& output, const RectF& input) {
    if (output.Width <= 0 || output.Height <= 0 || input.Width <= 0 || input.Height <= 0) return;
    for (const auto& row : source) {
        if (row.bounds.GetRight() <= input.X || row.bounds.X >= input.GetRight() ||
            row.bounds.GetBottom() <= input.Y || row.bounds.Y >= input.GetBottom()) continue;
        const float sx = output.Width / input.Width, sy = output.Height / input.Height;
        const RectF bounds(output.X + (row.bounds.X - input.X) * sx, output.Y + (row.bounds.Y - input.Y) * sy,
            row.bounds.Width * sx, row.bounds.Height * sy);
        PutText(destination, {row.text, bounds, row.color});
    }
}

static void RecordImageDraw(GpGraphics* graphics, GpImage* image, RectF output, RectF input, bool fullImage = false) {
    SourceLock lock;
    if (!lock) return;
    auto* destination = GraphicsTexts(graphics);
    if (!destination) return;
    const auto found = imageTexts.find(image);
    if (found == imageTexts.end()) return;
    const auto source = found->second;
    if (fullImage) {
        UINT width = 0, height = 0;
        if (GdipGetImageWidth(image, &width) != Ok || GdipGetImageHeight(image, &height) != Ok) return;
        input = RectF(0, 0, static_cast<REAL>(width), static_cast<REAL>(height));
        if (output.Width == 0 && output.Height == 0) { output.Width = input.Width; output.Height = input.Height; }
    }
    CopyTexts(source, *destination, output, input);
}

static void RecordHbitmap(GpImage* image, HBITMAP bitmap) {
    SourceLock lock;
    if (!lock) return;
    const auto found = imageTexts.find(image);
    if (found != imageTexts.end()) bitmapTexts[bitmap] = found->second;
}

static void RecordBitmapDraw(HDC sourceDC, HDC destinationDC, const RectF& output, const RectF& input) {
    SourceLock lock;
    if (!lock) return;
    const auto sourceBitmap = static_cast<HBITMAP>(GetCurrentObject(sourceDC, OBJ_BITMAP));
    const auto found = bitmapTexts.find(sourceBitmap);
    if (found == bitmapTexts.end()) return;
    const auto source = found->second;
    const auto destinationBitmap = static_cast<HBITMAP>(GetCurrentObject(destinationDC, OBJ_BITMAP));
    if (submittedBitmaps.erase(destinationBitmap)) bitmapTexts[destinationBitmap].clear();
    CopyTexts(source, bitmapTexts[destinationBitmap], output, input);
}

// 调用方持有 stateLock；像素只确认该位置确实有字，不再用字形相似度判断文字。
static const BitmapText* DrawnText(HBITMAP bitmap, const RectF& band) {
    const auto surface = bitmapTexts.find(bitmap);
    if (surface == bitmapTexts.end()) return nullptr;
    const BitmapText* chosen = nullptr;
    float distance = 1000;
    for (const auto& row : surface->second) {
        const float center = row.bounds.Y + row.bounds.Height / 2;
        const float delta = std::abs(center - band.Y - band.Height / 2);
        if (delta > row.bounds.Height / 2 + 3 || row.bounds.GetRight() + 2 < band.X || row.bounds.X - 2 > band.GetRight()) continue;
        if (delta < distance) { distance = delta; chosen = &row; }
    }
    return chosen;
}

static decltype(&GdipDeleteGraphics) originalDeleteGraphics;
static GpStatus WINGDIPAPI CaptureDeleteGraphics(GpGraphics* graphics) {
    {
        SourceLock lock(true);
        if (lock) graphicsSources.erase(graphics);
    }
    return originalDeleteGraphics(graphics);
}

static decltype(&GdipDisposeImage) originalDisposeImage;
static GpStatus WINGDIPAPI CaptureDisposeImage(GpImage* image) {
    {
        SourceLock lock(true);
        if (lock) imageTexts.erase(image);
    }
    return originalDisposeImage(image);
}

static decltype(&GdipGraphicsClear) originalClearGraphics;
static GpStatus WINGDIPAPI CaptureClearGraphics(GpGraphics* graphics, ARGB color) {
    const auto result = originalClearGraphics(graphics, color);
    if (result == Ok) {
        SourceLock lock;
        if (lock) {
            if (auto* rows = GraphicsTexts(graphics)) rows->clear();
        }
    }
    return result;
}

static decltype(&DeleteObject) originalDeleteObject;
static BOOL WINAPI CaptureDeleteObject(HGDIOBJ object) {
    const auto result = originalDeleteObject(object);
    if (result) {
        SourceLock lock(true);
        if (lock) {
            bitmapTexts.erase(static_cast<HBITMAP>(object));
            submittedBitmaps.erase(static_cast<HBITMAP>(object));
        }
    }
    return result;
}
