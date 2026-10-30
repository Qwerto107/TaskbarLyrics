#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>

// 每进程独立环形缓冲。槽位用奇偶序号防止读取到一半更新的数据；绘制线程从不等待读端。
constexpr int SlotCount = 256;
constexpr int TextCapacity = 512;
constexpr int ProtocolVersion = 19;
#pragma pack(push, 4)
struct DrawSample {
    volatile LONG guard;
    DWORD sequence;
    DWORD tick;
    DWORD color;
    DWORD kind; // 0 文字，1 路径填充，4 最终像素边界，7 诊断区域，8 完整帧（x 为直接关联行数），9 绘制关联诊断。
    float x, y, width, height;
    float clipX, clipY, clipWidth, clipHeight;
    WCHAR text[TextCapacity];
};
struct Channel {
    LONG version;
    volatile LONG enabled;
    volatile LONG latest;
    volatile LONG ready;
    DrawSample slots[SlotCount];
};
#pragma pack(pop)
static_assert(sizeof(DrawSample) == 1076);
inline void ChannelName(WCHAR* buffer, size_t capacity, DWORD pid) {
    swprintf_s(buffer, capacity, L"Local\\TaskbarLyrics.Draw.v%d.%lu", ProtocolVersion, pid);
}
