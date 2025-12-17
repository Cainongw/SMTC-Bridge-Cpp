# SMTC-Bridge-Cpp

**This repository is no longer maintained. For related new features and updates, please check [smtc_bridge_rust](https://github.com/Cainongw/smtc_bridge_rust)**

English | [简体中文](https://github.com/Cainongw/SMTC-Bridge-Cpp/tree/master/zh-cn)

**SMTCBridge** is a lightweight C++ Dynamic Link Library (DLL) designed to bridge Windows System Media Transport Controls (SMTC) and master volume control functionality to non-native languages such as C# (for Unity/BepInEx modding) and Python.

# Features

- **Media Information Retrieval (SMTC)**  
  Retrieve the title, artist, playback timeline (position and duration), and raw album cover image data of the currently active media session.

- **Event-Driven Architecture**  
  The library uses C-style callback functions to proactively notify the external environment when media properties (such as song title or artist) change, eliminating the need for polling.

- **Playback Control**  
  Provides media control commands including Play/Pause, Next Track, and Previous Track.

- **System Volume Control (Core Audio)**  
  Exposes safe interfaces for controlling the system master volume (Volume Up/Down).

# Build Requirements

To build this library, the following requirements must be met:

- **Operating System**: Windows 10 / 11  
- **SDK**: Windows SDK (target SDK 10.0.19041.0 or newer is recommended)  
- **Compiler**: Visual Studio 2019 / 2022 (with C++17 or later support)  
- **Dependencies**: C++/WinRT headers, `runtimeobject.lib`, and related system libraries

# Exported API Reference

## Lifecycle and Callbacks

| Function | Description |
|---|---|
| InitSMTC() | Starts the internal worker thread. |
| ShutdownSMTC() | Stops the thread and releases all resources. |
| RegisterUpdateCallback(SMTC_UpdateCallback callback) | Registers a callback function (e.g. from C#). |

## Media Operations

| Function | Description |
|---|---|
| SMTC_GetTitle(char* buffer, int len) | Get the current track title |
| SMTC_GetPlaybackStatus() | Get the current playback status |
| SMTC_Play() | Sends a play command. Asynchronously requests the current session to start playback |
| SMTC_Pause() | Sends a pause command. Asynchronously requests the current session to pause playback |
| SMTC_PlayPause() | Toggles between play and pause |
| SMTC_Next() | Sends the next-track command |
| SMTC_Previous() | Sends the previous-track command |
| SMTC_VolumeUp() | Increase system volume by 5% |
| SMTC_VolumeDown() | Decrease system volume by 5% |
| SMTC_SetVolume(float volume) | Set the system volume directly (0.0 – 1.0) |
SMTC_SetTimeline(long long positionTicks)| Set the current timeline|

# Usage

## 1. Build and Deployment

Build the project as an **x64 DLL** (for example, `SMTCBridge.dll`).

**Unity / BepInEx mod placement path**:

BepInEx/plugins/YourModName/x86_64/


---

## 3. C# Usage Example (for Unity Mods)

Use `DllImport` and `MarshalAs` to call the exported C APIs via P/Invoke.

```csharp
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

/// <summary>
/// Interop wrapper and event dispatcher for the C++ SMTC DLL.
/// Note: In Unity or other applications with a UI thread,
/// you need a mechanism to capture the main thread's SynchronizationContext.
/// </summary>
public sealed class SMTCWrapper : IDisposable
{
    // --- 1. DllImport declarations for exported C++ functions ---
    
    private const string DllName = "SafeSMTC";
    private const CallingConvention NativeCall = CallingConvention.Cdecl;

    // Matches the C++ enum SMTC_EventType
    public enum SMTC_EventType
    {
        MediaPropertiesChanged = 0, // Title, Artist, Cover changed
        TimelineChanged = 1,        // Position, Duration changed
        PlaybackStatusChanged = 2,  // Playback state changed
        SessionChanged = 3          // Media session switched (e.g. player change)
    }

    // Matches the C++ callback signature: void(__stdcall*)(SMTC_EventType eventType)
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void SMTC_UpdateCallback(SMTC_EventType eventType);

    // Lifecycle
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern void InitSMTC();
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern void ShutdownSMTC();

    // Callback registration
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern void RegisterUpdateCallback(SMTC_UpdateCallback callback);

    // Getters
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern int SMTC_GetTitle(byte[] buffer, int len);
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern int SMTC_GetArtist(byte[] buffer, int len);
    
    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern bool SMTC_GetPlaybackStatus();

    [DllImport(DllName, CallingConvention = NativeCall)]
    private static extern void SMTC_GetTimeline(out long position, out long duration);

    // Controls
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

    [DllImport(DllName, CallingConvention = NativeCall)]
    public static extern void SMTC_SetTimeline(long postionTicks);
}
```

⚠️ Notes

✔ Resource Cleanup

You must call ShutdownSMTC() when the application or mod exits.
This ensures that WinRT event listeners are properly unregistered and resources are released.
Failure to do so may result in hanging threads, application freezes, or crashes.

✔ Character Encoding

All exported strings are char* (ANSI / UTF-8).
Make sure to handle them as UTF-8 on the C# or Python side.
For example, in Python use .decode("utf-8").
