

// --- 1. 标准库头文件 ---
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <iomanip>

// --- 2. Windows 基础头文件 ---
#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// --- 3. C++/WinRT 头文件 ---
// 必须安装 Microsoft.Windows.CppWinRT NuGet 包并生成一次项目
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

// --- 4. 命名空间引用 ---
using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;
using namespace Windows::Foundation;

// ================= 全局变量定义 =================
std::atomic<bool> g_isInitialized{ false };
std::mutex g_dataMutex;
GlobalSystemMediaTransportControlsSessionManager g_manager = nullptr;
GlobalSystemMediaTransportControlsSession g_currentSession = nullptr;

// 缓存的数据
std::string g_title = "";
std::string g_artist = "";
std::vector<uint8_t> g_coverBuffer;
int64_t g_positionTicks = 0;
int64_t g_durationTicks = 0;
bool g_isPlaying = false;
bool g_hasNewCover = false;

static winrt::event_token g_sessionChangedToken;
static winrt::event_token g_mediaPropertiesToken;
static winrt::event_token g_timelinePropertiesToken;
static winrt::event_token g_playbackInfoToken;

// ================= 内部辅助函数 =================

void OnSessionManagerChanged();

std::string WinRTStringToString(hstring const& hstr) {
    return to_string(hstr);
}

void worker() {
    try {
        // 延时确保 WinRT Runtime + Explorer + SMTC 都初始化完毕
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        init_apartment();

        auto op = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        while (op.Status() == AsyncStatus::Started) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        g_manager = op.GetResults();
        if (!g_manager) {
            return;
        }


        auto sessions = g_manager.GetSessions();

        g_sessionChangedToken =
            g_manager.CurrentSessionChanged([](auto&&, auto&&) { OnSessionManagerChanged(); });

        OnSessionManagerChanged();
    }
    catch (...) {
    }
}
// 异步更新媒体属性 (歌名、封面)
fire_and_forget UpdateMediaProperties(GlobalSystemMediaTransportControlsSession session) {
    try {
        auto props = co_await session.TryGetMediaPropertiesAsync();
        if (!props) co_return;

        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_title = WinRTStringToString(props.Title());
        g_artist = WinRTStringToString(props.Artist());

        // 处理封面
        auto thumbRef = props.Thumbnail();
        if (thumbRef) {
            auto stream = co_await thumbRef.OpenReadAsync();
            if (stream) {
                // >>>>> 修正点在这里：直接传入 stream，不需要 GetInputStream() <<<<<
                auto reader = DataReader(stream);

                uint32_t size = (uint32_t)stream.Size();
                co_await reader.LoadAsync(size);

                g_coverBuffer.resize(size);
                reader.ReadBytes(g_coverBuffer);
                g_hasNewCover = true;
            }
        }
        else {
            g_coverBuffer.clear();
            g_hasNewCover = false;
        }
    }
    catch (...) {}
}

// 更新时间轴
void UpdateTimeline(GlobalSystemMediaTransportControlsSession session) {
    auto timeline = session.GetTimelineProperties();
    if (timeline) {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_positionTicks = timeline.Position().count();
        g_durationTicks = timeline.EndTime().count();
    }
}

// 更新播放状态
void UpdatePlaybackInfo(GlobalSystemMediaTransportControlsSession session) {
    auto info = session.GetPlaybackInfo();
    if (info) {
        std::lock_guard<std::mutex> lock(g_dataMutex);
        g_isPlaying = (info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
    }
}

// 设置事件监听
void SetupSessionEvents(GlobalSystemMediaTransportControlsSession session) {
    if (!session) return;

    // 确保在重新挂钩前解除旧的监听 (如果 session 变了)
    if (g_currentSession) {
        // 尝试解除旧 token，防止内存泄漏
        try { g_currentSession.MediaPropertiesChanged(g_mediaPropertiesToken); }
        catch (...) {}
        try { g_currentSession.TimelinePropertiesChanged(g_timelinePropertiesToken); }
        catch (...) {}
        try { g_currentSession.PlaybackInfoChanged(g_playbackInfoToken); }
        catch (...) {}
    }

    // 存储新的 token
    g_mediaPropertiesToken = session.MediaPropertiesChanged([](auto&& s, auto&&) { UpdateMediaProperties(s); });
    g_timelinePropertiesToken = session.TimelinePropertiesChanged([](auto&& s, auto&&) { UpdateTimeline(s); });
    g_playbackInfoToken = session.PlaybackInfoChanged([](auto&& s, auto&&) { UpdatePlaybackInfo(s); });

    // 立即触发一次更新
    UpdateMediaProperties(session);
    UpdateTimeline(session);
    UpdatePlaybackInfo(session);
}

// 会话改变回调
static std::atomic<bool> g_isChangingSession{ false };

void OnSessionManagerChanged() {
    // 如果已经在处理会话改变，直接返回
    if (g_isChangingSession.exchange(true)) return;

    try {
        GlobalSystemMediaTransportControlsSession bestSession = nullptr;

        auto sessions = g_manager.GetSessions();
        for (auto const& s : sessions) {
            auto info = s.GetPlaybackInfo();
            if (info && info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                bestSession = s; // 优先选择正在播放的
                break;
            }
        }

        if (!bestSession) {
            // 回退到 focused session
            bestSession = g_manager.GetCurrentSession();
        }

        if (bestSession) {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_currentSession = bestSession;
            SetupSessionEvents(g_currentSession);
        }
    }
    catch (...) {}

    g_isChangingSession.store(false);
}

// ================= 导出接口 (extern "C") =================

extern "C" __declspec(dllexport) void InitSMTC() {
    bool expected = false;
    if (g_isInitialized.compare_exchange_strong(expected, true)) {
        // 第一次调用，启动 worker 线程
        std::thread(worker).detach();
    }
    else {
        return;
    }
}
extern "C" __declspec(dllexport) void SMTC_Play() {
    if (g_currentSession) g_currentSession.TryPlayAsync();
}

extern "C" __declspec(dllexport) void SMTC_Pause() {
    if (g_currentSession) g_currentSession.TryPauseAsync();
}
extern "C" __declspec(dllexport) void SMTC_PlayPause() {
    if (g_currentSession) g_currentSession.TryTogglePlayPauseAsync();
}

extern "C" __declspec(dllexport) void SMTC_Next() {
    if (g_currentSession) g_currentSession.TrySkipNextAsync();
}

extern "C" __declspec(dllexport) void SMTC_Previous() {
    if (g_currentSession) g_currentSession.TrySkipPreviousAsync();
}

extern "C" __declspec(dllexport) void SMTC_VolumeUp() {
    keybd_event(VK_VOLUME_UP, 0, 0, 0);
    keybd_event(VK_VOLUME_UP, 0, 2, 0);
}

extern "C" __declspec(dllexport) void SMTC_VolumeDown() {
    keybd_event(VK_VOLUME_DOWN, 0, 0, 0);
    keybd_event(VK_VOLUME_DOWN, 0, 2, 0);
}

// --- 数据读取 ---

extern "C" __declspec(dllexport) int SMTC_GetTitle(char* buffer, int length) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    if (g_title.empty()) return 0;
    if (buffer && length > 0) strncpy_s(buffer, length, g_title.c_str(), _TRUNCATE);
    return (int)g_title.length();
}

extern "C" __declspec(dllexport) int SMTC_GetArtist(char* buffer, int length) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    if (g_artist.empty()) return 0;
    if (buffer && length > 0) strncpy_s(buffer, length, g_artist.c_str(), _TRUNCATE);
    return (int)g_artist.length();
}

extern "C" __declspec(dllexport) bool SMTC_GetPlaybackStatus() {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    return g_isPlaying;
}

extern "C" __declspec(dllexport) void SMTC_GetTimeline(int64_t* position, int64_t* duration) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    if (position) *position = g_positionTicks;
    if (duration) *duration = g_durationTicks;
}

extern "C" __declspec(dllexport) int SMTC_GetCoverImage(uint8_t* buffer, int length) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    if (g_coverBuffer.empty()) return 0;

    if (buffer == nullptr) {
        return g_hasNewCover ? (int)g_coverBuffer.size() : 0;
    }

    if (length >= (int)g_coverBuffer.size()) {
        memcpy(buffer, g_coverBuffer.data(), g_coverBuffer.size());
        g_hasNewCover = false;
        return (int)g_coverBuffer.size();
    }
    return -1;
}
extern "C" __declspec(dllexport) void ShutdownSMTC() {
    try {
        // 1. 解除会话管理器事件
        if (g_manager && g_sessionChangedToken.value != 0) {
            // 这是唯一一个必须手动解除，因为它是在 worker 线程中创建的。
            g_manager.CurrentSessionChanged(g_sessionChangedToken);
            g_sessionChangedToken = winrt::event_token{};
        }

        // 2. 清理全局 WinRT 对象 (依赖 RAII 自动处理 g_currentSession 的所有事件)
        g_currentSession = nullptr; // 释放 WinRT 对象，其内部资源（包括事件）应被释放。
        g_manager = nullptr;


    }
    catch (...) {
        // 忽略清理时的异常，确保程序能退出
    }
}