# SMTC-Bridge-Cpp

English | 简体中文

**SMTCBridge** 是一个轻量级的 C++ 动态链接库（DLL），用于将 Windows 的系统媒体传输控制（SMTC）以及主音量控制功能桥接到非原生语言（如 C# —— 用于 Unity/BepInEx Modding）和 Python。

# 功能特性

- **媒体信息获取（SMTC）**  
  获取当前活动媒体会话的标题、艺术家、播放进度时间轴、封面图像原始数据。

- **事件驱动**
  库使用 C-style 回调函数，在媒体属性（歌曲名、艺术家）发生变化时，主动通知外部环境，无需轮询。

- **播放控制**  
  提供媒体控制命令，例如：播放/暂停、下一曲、上一曲

- **系统音量控制（Core Audio）**  
  提供了系统音量控制 (Volume Up/Down) 的安全导出接口。

# 构建要求

编译此库需要满足以下条件：

- **操作系统**：Windows 10/11  
- **SDK**：Windows SDK（推荐使用目标 SDK 10.0.19041.0 或更新版本）  
- **编译器**：Visual Studio 2019/2022（需支持 C++17 或更高）  
- **依赖**：C++/WinRT 头文件、runtimeobject.lib 等链接库

# 导出接口信息

## 生命周期与回调

|函数|描述|
|---|---|
|InitSMTC()|启动 Worker 线程。|
|ShutdownSMTC()|停止线程并清理资源。|
|RegisterUpdateCallback(SMTC_UpdateCallback callback)|注册 C# 回调函数。|

## 媒体操作

|函数|描述|
---|---|
|SMTC_GetTitle(char* buffer, int len)|获取当前歌曲名|
|SMTC_GetPlaybackStatus()|获取播放状态 |
|SMTC_Play()|单独发送播放命令。异步请求当前 Session 开始播放 |
|SMTC_Pause()|单独发送暂停命令。异步请求当前 Session 暂停播放|
|SMTC_PlayPause()|切换播放/暂停状态|
|SMTC_Next()|发送下一首命令|
|SMTC_Previous()|发送上一首命令|
|SMTC_VolumeUp()|增加系统音量 (5%)|
|SMTC_VolumeUp()|降低系统音量 (5%)|
|SMTC_SetVolume(float volume)|直接设置系统音量(0.0-1.0)|

# 使用

## 1. 编译与部署

将项目编译为 **x64 架构 DLL**（例如 `SMTCBridge.dll`）。

**Unity/BepInEx Modding 放置位置**：

BepInEx/plugins/YourModName/x86_64/

---


## 3. C# 调用示例（用于 Unity Mod）

使用 `DllImport` 和 `MarshalAs` 调用导出的 C API。(P/Invoke)

```csharp
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

/// <summary>
/// 封装 C++ SMTC DLL 的互操作和事件调度。
/// 注意: 在 Unity 或其他具有 UI 线程的应用中，你需要有一个机制来捕获主线程的 SynchronizationContext。
/// </summary>
public sealed class SMTCWrapper : IDisposable
{
    // --- 1. C++ 导出函数的 DllImport 声明 ---
    
    private const string DllName = "SafeSMTC";
    private const CallingConvention NativeCall = CallingConvention.Cdecl;

    // 匹配 C++ enum SMTC_EventType
    public enum SMTC_EventType
    {
        MediaPropertiesChanged = 0, // Title, Artist, Cover 变化
        TimelineChanged = 1,        // Position, Duration 变化
        PlaybackStatusChanged = 2,  // 播放状态变化
        SessionChanged = 3          // Session 切换 (如切换播放器)
    }

    // 匹配 C++ 回调函数签名: void(__stdcall*)(SMTC_EventType eventType)
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void SMTC_UpdateCallback(SMTC_EventType eventType);

    // DllImport 声明: 生命周期
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern void InitSMTC();
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern void ShutdownSMTC();

    // DllImport 声明: 回调注册
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern void RegisterUpdateCallback(SMTC_UpdateCallback callback);

    // DllImport 声明: Getter
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern int SMTC_GetTitle(byte[] buffer, int len);
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern int SMTC_GetArtist(byte[] buffer, int len);
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern bool SMTC_GetPlaybackStatus();

    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern void SMTC_GetTimeline(out long position, out long duration);

    // DllImport 声明: 控制
    [DllImport(DllName, CallingConvention = NativeCall)]
    public static extern void SMTC_Play();
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    public static extern void SMTC_Pause();
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    public static extern void SMTC_Next();

    [DllImport(DllName, CallingConvention = NativeCall)]
    public static extern void SMTC_VolumeUp();
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    public static extern void SMTC_SetVolume(float volume);
}
```

## ⚠️ 注意事项
✔ 资源清理

必须在应用或 Mod 退出时调用 ShutdownSMTC()，负责卸载 WinRT 事件监听器，释放资源。
否则可能导致线程悬挂、程序卡死或崩溃。


✔ 字符编码

导出的字符串为 char*（ANSI/UTF-8），请在 C# 或 Python 端按 UTF-8 处理。
例如 Python 中用 .decode('utf-8')。