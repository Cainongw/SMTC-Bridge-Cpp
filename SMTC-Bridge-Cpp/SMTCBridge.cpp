// SafeSMTC.cpp — 重构后安全版本
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <iostream>
#include <algorithm>
#include <queue>
#include <condition_variable>
#include <functional>
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>


#include <mmdeviceapi.h>
#include <endpointvolume.h>
#pragma comment(lib, "Ole32.lib")

using namespace winrt;
using namespace Windows::Media::Control;
using namespace Windows::Storage::Streams;
using namespace Windows::Foundation;

// ================= 全局控制变量 =================
// 这些数据可以被外部（主线程/Unity）读，但所有 WinRT 操作必须在 worker 线程内完成。
static std::atomic<bool> g_isRunning{ false };
static std::thread g_workerThread;

// 线程安全数据保护
static std::mutex g_dataMutex;
static std::string g_title;
static std::string g_artist;
static std::vector<uint8_t> g_coverBuffer;
static int64_t g_positionTicks = 0;
static int64_t g_durationTicks = 0;
static bool g_isPlaying = false;
static bool g_hasNewCover = false;

// WinRT 管理对象及 event token —— 仅在 worker 线程中使用/访问
static GlobalSystemMediaTransportControlsSessionManager g_manager = nullptr;
static GlobalSystemMediaTransportControlsSession g_currentSession = nullptr;
static winrt::event_token g_sessionChangedToken{};
static winrt::event_token g_mediaPropertiesToken{};
static winrt::event_token g_timelinePropertiesToken{};
static winrt::event_token g_playbackInfoToken{};

// 任务队列： WinRT 回调会把“工作单元” push 到此队列，真正的处理在 worker 线程执行。
static std::mutex g_queueMutex;
static std::condition_variable g_queueCv;
static std::queue<std::function<void()>> g_taskQueue;

// 防止并发切换 session 标记（仅在 worker 线程里检查和操作）
static bool g_isChangingSession = false;
// ================= Core Audio helper =================
// 获取默认输出设备的 IAudioEndpointVolume 接口（调用者负责 Release）
static IAudioEndpointVolume* GetEndpointVolume() {
    HRESULT hr = S_OK;
    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioEndpointVolume* pVolume = nullptr;

    // 使用 CoCreateInstance 创建 IMMDeviceEnumerator（线程已经 init_apartment，winrt::init_apartment 已调用）
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr) || !pEnumerator) {
        if (pEnumerator) pEnumerator->Release();
        return nullptr;
    }

    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    pEnumerator->Release();
    if (FAILED(hr) || !pDevice) {
        if (pDevice) pDevice->Release();
        return nullptr;
    }

    hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, (void**)&pVolume);
    pDevice->Release();
    if (FAILED(hr) || !pVolume) {
        if (pVolume) pVolume->Release();
        return nullptr;
    }

    return pVolume; // caller must Release()
}

// 调整音量：delta 为相对变化（范围 -1.0 .. +1.0）
static void ChangeSystemVolumeBy(double delta) {
    IAudioEndpointVolume* pVolume = GetEndpointVolume();
    if (!pVolume) return;

    float current = 0.0f;
    HRESULT hr = pVolume->GetMasterVolumeLevelScalar(&current);
    if (SUCCEEDED(hr)) {
        double target = static_cast<double>(current) + delta;
        if (target < 0.0) target = 0.0;
        if (target > 1.0) target = 1.0;
        // SetMasterVolumeLevelScalar 需要 float (0.0~1.0), pGUID 可为 nullptr 表示系统范围改变
        pVolume->SetMasterVolumeLevelScalar(static_cast<float>(target), nullptr);
    }

    pVolume->Release();
}


// ================= 辅助函数 =================
static std::string WinRTStringToString(hstring const& hstr) {
    return winrt::to_string(hstr);
}

// 将任务推入队列并通知 worker
static void EnqueueTask(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(g_queueMutex);
        g_taskQueue.push(std::move(task));
    }
    g_queueCv.notify_one();
}

// 安全地注销当前 session 的事件 token —— 必须在 worker 线程执行
static void UnregisterCurrentSessionEvents() {
    try {
        if (g_currentSession) {
            if (g_mediaPropertiesToken.value) {
                try { g_currentSession.MediaPropertiesChanged(g_mediaPropertiesToken); }
                catch (...) {}
                g_mediaPropertiesToken = {};
            }
            if (g_timelinePropertiesToken.value) {
                try { g_currentSession.TimelinePropertiesChanged(g_timelinePropertiesToken); }
                catch (...) {}
                g_timelinePropertiesToken = {};
            }
            if (g_playbackInfoToken.value) {
                try { g_currentSession.PlaybackInfoChanged(g_playbackInfoToken); }
                catch (...) {}
                g_playbackInfoToken = {};
            }
        }
    }
    catch (...) {}
}

// Update 函数：这些函数**假定**在 worker 线程中被调用（所以它们可以安全调用 WinRT API）
fire_and_forget UpdateMediaProperties(GlobalSystemMediaTransportControlsSession session) {
    // 注意：这个 coroutine 必须在已经 init_apartment 的线程上被调用 —— 所以我们确保它在 worker 线程执行
    try {
        auto strongSession = session;
        auto props = co_await strongSession.TryGetMediaPropertiesAsync();

        if (!props) co_return;

        {
            std::lock_guard<std::mutex> lk(g_dataMutex);
            g_title = WinRTStringToString(props.Title());
            g_artist = WinRTStringToString(props.Artist());
        }

        auto thumbRef = props.Thumbnail();
        if (thumbRef) {
            auto stream = co_await thumbRef.OpenReadAsync();
            if (stream) {
                DataReader reader(stream);
                uint32_t size = static_cast<uint32_t>(stream.Size());
                co_await reader.LoadAsync(size);

                std::vector<uint8_t> localBuf(size);
                reader.ReadBytes(localBuf);

                {
                    std::lock_guard<std::mutex> lk(g_dataMutex);
                    g_coverBuffer = std::move(localBuf);
                    g_hasNewCover = true;
                }
            }
        }
        else {
            std::lock_guard<std::mutex> lk(g_dataMutex);
            g_coverBuffer.clear();
            g_hasNewCover = false;
        }
    }
    catch (...) {
        // 忽略异常但不要抛出
    }
}

static void UpdateTimeline_Internal(GlobalSystemMediaTransportControlsSession session) {
    try {
        auto timeline = session.GetTimelineProperties();
        if (timeline) {
            std::lock_guard<std::mutex> lk(g_dataMutex);
            g_positionTicks = timeline.Position().count();
            g_durationTicks = timeline.EndTime().count();
        }
    }
    catch (...) {}
}

static void UpdatePlaybackInfo_Internal(GlobalSystemMediaTransportControlsSession session) {
    try {
        auto info = session.GetPlaybackInfo();
        if (info) {
            std::lock_guard<std::mutex> lk(g_dataMutex);
            g_isPlaying = (info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
        }
    }
    catch (...) {}
}

// 在 worker 线程中注册 session 的事件（事件 handler 只把任务 push 回 worker）
static void SetupSessionEvents_Internal(GlobalSystemMediaTransportControlsSession session) {
    if (!session) return;

    // 使用 weak_ref 防止 lambda 持有强引用导致循环引用
    winrt::weak_ref<GlobalSystemMediaTransportControlsSession> weakSession{ session };

    // 像下面的 lambda **只做一个事**：把实际工作推入队列（在 worker 线程执行）
    g_mediaPropertiesToken = session.MediaPropertiesChanged([weakSession](auto&& s, auto&&) {
        // 注意：不要在这里直接调用 WinRT API
        EnqueueTask([weakSession]() {
            if (auto strong = weakSession.get()) {
                // call coroutine on worker thread
                UpdateMediaProperties(strong);
            }
            });
        });

    g_timelinePropertiesToken = session.TimelinePropertiesChanged([weakSession](auto&& s, auto&&) {
        EnqueueTask([weakSession]() {
            if (auto strong = weakSession.get()) {
                UpdateTimeline_Internal(strong);
            }
            });
        });

    g_playbackInfoToken = session.PlaybackInfoChanged([weakSession](auto&& s, auto&&) {
        EnqueueTask([weakSession]() {
            if (auto strong = weakSession.get()) {
                UpdatePlaybackInfo_Internal(strong);
            }
            });
        });

    // 启动一次初始读取（在 worker 线程执行）
    EnqueueTask([weakSession]() {
        if (auto strong = weakSession.get()) {
            UpdateMediaProperties(strong);
            UpdateTimeline_Internal(strong);
            UpdatePlaybackInfo_Internal(strong);
        }
        });
}

// OnSessionManagerChanged 的实际逻辑（必须在 worker 线程中执行）
static void OnSessionManagerChanged_Internal() {
    if (!g_manager) return;
    if (g_isChangingSession) return;
    g_isChangingSession = true;

    // 1. 注销旧 session 的事件（在 worker 线程中安全）
    UnregisterCurrentSessionEvents();
    g_currentSession = nullptr;

    // 2. 选择最好的 session（和你原有逻辑类似）
    GlobalSystemMediaTransportControlsSession bestSession = nullptr;
    try {
        auto sessions = g_manager.GetSessions();
        for (auto const& s : sessions) {
            try {
                auto info = s.GetPlaybackInfo();
                if (info && info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                    bestSession = s;
                    break;
                }
            }
            catch (...) {}
        }
        if (!bestSession) {
            bestSession = g_manager.GetCurrentSession();
        }
    }
    catch (...) {}

    // 3. 保存并注册新 session 的事件（仍然在 worker 线程中）
    g_currentSession = bestSession;
    if (g_currentSession) {
        SetupSessionEvents_Internal(g_currentSession);
    }

    g_isChangingSession = false;
}

// Worker 主循环：处理队列任务 & 管理 manager/session 的生命周期
static void WorkerThreadFunc() {
    // 初始化 COM/WinRT apartment
    init_apartment(); // MTA by default

    // 请求 manager（必须在 worker 线程）
    try {
        auto op = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        // 等待完成（但我们不用忙等）
        while (op.Status() == AsyncStatus::Started && g_isRunning.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!g_isRunning.load()) {
            // 提前退出
            uninit_apartment();
            return;
        }

        g_manager = op.GetResults();
        if (!g_manager) {
            // 失败，退出
            uninit_apartment();
            return;
        }

        // 注册 manager.CurrentSessionChanged 事件：事件 handler 仅 enqueue 一个任务
        winrt::weak_ref<GlobalSystemMediaTransportControlsSessionManager> weakManager{ g_manager };
        g_sessionChangedToken = g_manager.CurrentSessionChanged([weakManager](auto&&, auto&&) {
            EnqueueTask([weakManager]() {
                if (auto mgr = weakManager.get()) {
                    // 这里我们直接调用内部处理函数（在 worker 线程安全）
                    OnSessionManagerChanged_Internal();
                }
                });
            });

        // 首次执行一次
        EnqueueTask([]() {
            OnSessionManagerChanged_Internal();
            });

        // 主循环：等待任务并执行，直到 g_isRunning = false
        while (g_isRunning.load()) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(g_queueMutex);
                if (g_taskQueue.empty()) {
                    // 等待或被通知退出
                    g_queueCv.wait_for(lk, std::chrono::milliseconds(200));
                }
                if (!g_taskQueue.empty()) {
                    task = std::move(g_taskQueue.front());
                    g_taskQueue.pop();
                }
            }

            if (task) {
                try {
                    task(); // 在 worker 线程内执行所有 WinRT 操作
                }
                catch (...) {
                }
            }
        }

        // 退出前清理：注销 session 和 manager 事件（在 worker 线程）
        try {
            UnregisterCurrentSessionEvents();
            if (g_sessionChangedToken.value) {
                try { g_manager.CurrentSessionChanged(g_sessionChangedToken); }
                catch (...) {}
                g_sessionChangedToken = {};
            }
        }
        catch (...) {}

        g_manager = nullptr;
        g_currentSession = nullptr;
    }
    catch (...) {
        // 忽略错误
    }

    // WinRT uninit
    uninit_apartment();
}

// ================= 导出接口 =================
extern "C" __declspec(dllexport) void InitSMTC() {
    bool expected = false;
    if (!g_isRunning.compare_exchange_strong(expected, true)) {
        // 已经初始化
        return;
    }

    // 启动 worker 线程
    g_workerThread = std::thread([]() {
        WorkerThreadFunc();
        });
}

extern "C" __declspec(dllexport) void ShutdownSMTC() {
    bool expected = true;
    if (!g_isRunning.compare_exchange_strong(expected, false)) {
        // 已经停止或未启动
        return;
    }

    // 唤醒并等待线程退出
    g_queueCv.notify_one();
    if (g_workerThread.joinable()) {
        try {
            g_workerThread.join();
        }
        catch (...) {}
    }

    // 清理共享缓存（外部读者可能会读取这些变量）
    {
        std::lock_guard<std::mutex> lk(g_dataMutex);
        g_title.clear();
        g_artist.clear();
        g_coverBuffer.clear();
        g_positionTicks = 0;
        g_durationTicks = 0;
        g_isPlaying = false;
        g_hasNewCover = false;
    }
}


static void EnqueueControl(std::function<void(GlobalSystemMediaTransportControlsSession)> fn) {
    EnqueueTask([fn]() {
        if (g_currentSession) {
            try { fn(g_currentSession); }
            catch (...) {}
        }
        });
}

extern "C" __declspec(dllexport) void SMTC_PlayPause() {
    EnqueueControl([](auto session) {
        session.TryTogglePlayPauseAsync();
        });
}

extern "C" __declspec(dllexport) void SMTC_Play() {
    EnqueueControl([](auto session) {
        session.TryPlayAsync();
        });
}

extern "C" __declspec(dllexport) void SMTC_Pause() {
    EnqueueControl([](auto session) {
        session.TryPauseAsync();
        });
}

extern "C" __declspec(dllexport) void SMTC_Next() {
    EnqueueControl([](auto session) {
        session.TrySkipNextAsync();
        });
}

extern "C" __declspec(dllexport) void SMTC_Previous() {
    EnqueueControl([](auto session) {
        session.TrySkipPreviousAsync();
        });
}

// ================= Volume =================
extern "C" __declspec(dllexport) void SMTC_VolumeUp() {
    // 增加 5% 音量
    EnqueueTask([]() {
        try {
            ChangeSystemVolumeBy(0.05);
        }
        catch (...) {}
        });
}

extern "C" __declspec(dllexport) void SMTC_VolumeDown() {
    // 降低 5% 音量
    EnqueueTask([]() {
        try {
            ChangeSystemVolumeBy(-0.05);
        }
        catch (...) {}
        });
}




// ==========================================================
//  === 安全 Getter ===
// ==========================================================

// 写入 title/artist。返回写入的字节数。
extern "C" __declspec(dllexport) int SMTC_GetTitle(char* buffer, int len) {
    if (!buffer || len <= 0) return 0;
    std::lock_guard<std::mutex> lk(g_dataMutex);
    if (g_title.empty()) return 0;

    int copyLen = std::min<int>(len - 1, (int)g_title.size());
    memcpy(buffer, g_title.data(), copyLen);
    buffer[copyLen] = '\0';
    return copyLen;
}

extern "C" __declspec(dllexport) int SMTC_GetArtist(char* buffer, int len) {
    if (!buffer || len <= 0) return 0;
    std::lock_guard<std::mutex> lk(g_dataMutex);
    if (g_artist.empty()) return 0;

    int copyLen = std::min<int>(len - 1, (int)g_artist.size());
    memcpy(buffer, g_artist.data(), copyLen);
    buffer[copyLen] = '\0';
    return copyLen;
}


// 播放状态
extern "C" __declspec(dllexport) bool SMTC_GetPlaybackStatus() {
    std::lock_guard<std::mutex> lk(g_dataMutex);
    return g_isPlaying;
}


// 时间轴
extern "C" __declspec(dllexport) void SMTC_GetTimeline(long long* position, long long* duration) {
    if (!position || !duration) return;
    std::lock_guard<std::mutex> lk(g_dataMutex);
    *position = g_positionTicks;
    *duration = g_durationTicks;
}


// 封面：返回写入的字节数
extern "C" __declspec(dllexport) int SMTC_GetCoverImage(uint8_t* buffer, int len) {
    if (!buffer || len <= 0) return 0;

    std::lock_guard<std::mutex> lk(g_dataMutex);
    if (g_coverBuffer.empty()) return 0;

    int copyLen = std::min<int>(len, (int)g_coverBuffer.size());
    memcpy(buffer, g_coverBuffer.data(), copyLen);
    return copyLen;
}
