# SMTC-Bridge-Cpp

English | 简体中文

**SMTCBridge** 是一个轻量级的 C++ 动态链接库（DLL），用于将 Windows 的系统媒体传输控制（SMTC）以及主音量控制功能桥接到非原生语言（如 C# —— 用于 Unity/BepInEx Modding）和 Python。

# 功能特性

- **媒体信息获取（SMTC）**  
  获取当前活动媒体会话的标题、艺术家、播放进度时间轴、封面图像原始数据。

- **播放状态查询**  
  判断当前媒体是否正在播放（`SMTC_GetPlaybackStatus`）。

- **播放控制**  
  调用媒体控制命令，例如：播放/暂停（`SMTC_PlayPause`）、下一曲（`SMTC_Next`）、上一曲（`SMTC_Previous`）。

- **系统音量控制（Core Audio）**  
  使用模拟键盘事件实现系统主音量调节（`SMTC_VolumeUp`, `SMTC_VolumeDown`）。

- **轮询/查询式设计**  
  使用数据缓存 + 拉取机制，避免复杂的跨进程回调委托，使 C# 端集成更简单。

# 构建要求

编译此库需要满足以下条件：

- **操作系统**：Windows 10/11  
- **SDK**：Windows SDK（推荐使用目标 SDK 10.0.19041.0 或更新版本）  
- **编译器**：Visual Studio 2019/2022（需支持 C++17 或更高）  
- **依赖**：C++/WinRT 头文件、runtimeobject.lib 等链接库

# 🚀 集成与使用

## 1. 编译与部署

将项目编译为 **x64 架构 DLL**（例如 `SMTCBridge.dll`）。

**Unity/BepInEx Modding 放置位置**：

BepInEx/plugins/YourModName/x86_64/

---

## 2. C# 调用示例（用于 Unity Mod）

使用 `DllImport` 和 `MarshalAs` 调用导出的 C API。(P/Invoke)

```csharp
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SmtcBridge {
    private const string DllName = "SMTCBridge";

    // --- 控制与初始化 ---
    [DllImport(DllName)]
    public static extern void InitSMTC();

    [DllImport(DllName)]
    public static extern void ShutdownSMTC(); // Mod 退出时必须调用！

    [DllImport(DllName)]
    public static extern void SMTC_PlayPause();

    // --- 数据获取（注意缓冲区管理）---
    [DllImport(DllName, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
    public static extern int SMTC_GetTitle(StringBuilder buffer, int length);

    // 获取播放状态
    [DllImport(DllName)]
    [return: MarshalAs(UnmanagedType.I1)] // C++ bool（1字节）
    public static extern bool SMTC_GetPlaybackStatus();

    // C# 字符串获取辅助方法
    public static string GetCurrentTitle()
    {
        // 1. 获取所需缓冲区长度
        int length = SMTC_GetTitle(null, 0);
        if (length <= 0) return string.Empty;

        // 2. 填充缓冲区
        StringBuilder buffer = new StringBuilder(length + 1);
        SMTC_GetTitle(buffer, buffer.Capacity);

        return buffer.ToString();
    }
}
```

## 3. Python 调用示例（用于快速测试 / API 构建）

使用 ctypes 调用 DLL：
```Python
import ctypes
import atexit

DLL_PATH = "SMTCBridge.dll"
smtc_dll = ctypes.CDLL(DLL_PATH)

# 配置函数签名
smtc_dll.InitSMTC.restype = None
smtc_dll.SMTC_GetTitle.argtypes = [ctypes.c_char_p, ctypes.c_int]
smtc_dll.SMTC_GetTitle.restype = ctypes.c_int
smtc_dll.SMTC_GetPlaybackStatus.restype = ctypes.c_bool
smtc_dll.ShutdownSMTC.restype = None

# 程序退出时自动清理资源
atexit.register(lambda: smtc_dll.ShutdownSMTC())

# 字符串获取辅助函数
def get_title():
    # 1. 调用一次获取长度
    required_length = smtc_dll.SMTC_GetTitle(None, 0)
    if required_length <= 0:
        return ""

    # 2. 分配缓冲区并填充
    buffer_size = required_length + 1
    buffer = ctypes.create_string_buffer(buffer_size)
    smtc_dll.SMTC_GetTitle(buffer, buffer_size)

    return buffer.value.decode('utf-8', errors='ignore')

# 示例调用
smtc_dll.InitSMTC()

print(f"Current Title: {get_title()}")
print(f"Is Playing: {smtc_dll.SMTC_GetPlaybackStatus()}")
smtc_dll.SMTC_PlayPause()
```

## ⚠️ 注意事项
✔ 资源清理

必须在应用或 Mod 退出时调用 ShutdownSMTC()，负责卸载 WinRT 事件监听器，释放资源。
否则可能导致线程悬挂、程序卡死或崩溃。

✔ 线程安全

内部使用 std::mutex 保护缓存数据（如 g_title, g_isPlaying 等），确保异步 WinRT 后台线程与主线程之间的数据安全。

✔ 字符编码

导出的字符串为 char*（ANSI/UTF-8），请在 C# 或 Python 端按 UTF-8 处理。
例如 Python 中用 .decode('utf-8')。