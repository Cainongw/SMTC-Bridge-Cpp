# SMTC-Bridge-Cpp

English | [简体中文]()

SMTCBridge is a lightweight C++ Dynamic Link Library (DLL) designed to bridge Windows System Media Transport Controls (SMTC) and master volume control functionality to applications using non-native languages, such as C# (for Unity/BepInEx Modding) and Python.

# Features
Media Information Retrieval (SMTC): Fetch the title, artist, playback timeline, and raw cover image data from the active media session.

Playback Status: Query whether the media is currently playing (SMTC_GetPlaybackStatus).

Playback Controls: Send media commands like Play/Pause (SMTC_PlayPause), Next Track (SMTC_Next), and Previous Track (SMTC_Previous).

System Volume Control (Core Audio): Implement system master volume adjustment using simulated keyboard events (SMTC_VolumeUp, SMTC_VolumeDown).

Polling/Query Design: Utilizes a data caching and pulling mechanism, simplifying integration on the C# side by avoiding complex cross-process callback delegate management.

# Build Requirements
To successfully compile this library, your development environment must meet the following criteria:

Operating System: Windows 10/11

SDK: Windows SDK (Target SDK 10.0.19041.0 or newer recommended)

Compiler: Visual Studio 2019/2022 (with C++17 support or higher)

Dependencies: C++/WinRT headers, and linking libraries like runtimeobject.lib.

🚀 Integration and Usage
1. Compilation and Deployment
Compile the project as an x64 architecture DLL (e.g., SMTCBridge.dll).

Unity/BepInEx Modding: Place the compiled SMTCBridge.dll within your Mod directory, typically under BepInEx/plugins/YourModName/x86_64/.

2. C# Calling Example (Unity Mod)
You can use C#'s DllImport and MarshalAs to safely call the exported C API.

```C#

using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SmtcBridge { private const string DllName = "SMTCBridge";

    // --- Control and Initialization ---
    [DllImport(DllName)]
    public static extern void InitSMTC();
    [DllImport(DllName)] public static extern void ShutdownSMTC(); // Must be called when the Mod exits!

    [DllImport(DllName)]
    public static extern void SMTC_PlayPause();
    // --- Data Retrieval (Note on buffer management) ---
    // Returns string length (int) and fills the char* buffer [DllImport(DllName, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern int SMTC_GetTitle(StringBuilder buffer, int length);

    // Get playback status
    [DllImport(DllName)]
    [return: MarshalAs(UnmanagedType.I1)] // C++ bool (1 byte)
    public static extern bool SMTC_GetPlaybackStatus();
    // C# Helper Method (for safely retrieving strings)
    public static string GetCurrentTitle()
    {
        // 1. Call once to get the required length
        int length = SMTC_GetTitle(null, 0); 
        if (length <= 0) return string.Empty;
        // 2. Call again to fill the buffer
        StringBuilder buffer = new StringBuilder(length + 1);
        SMTC_GetTitle(buffer, buffer.Capacity);
        
        return buffer.ToString();
    }
}
```

### 3\. Python Calling Example (Quick Test/API Construction)
For Python, use the ctypes module (relevant to your goal of using Python for quick API building):

```Python
import ctypes
import atexit
DLL_PATH = "SMTCBridge.dll" smtc_dll = ctypes.CDLL(DLL_PATH)

Configure function signatures
smtc_dll.InitSMTC.restype = None smtc_dll.SMTC_GetTitle.argtypes = [ctypes.c_char_p, ctypes.c_int] smtc_dll.SMTC_GetTitle.restype = ctypes.c_int smtc_dll.SMTC_GetPlaybackStatus.restype = ctypes.c_bool smtc_dll.ShutdownSMTC.restype = None

Ensure ShutdownSMTC is called upon program exit
atexit.register(lambda: smtc_dll.ShutdownSMTC())

String retrieval helper function (to match C++ logic)
def get_title(): # 1. Call once to get the length required_length = smtc_dll.SMTC_GetTitle(None, 0) if required_length <= 0: return ""

# 2. Call again to fill the buffer
buffer_size = required_length + 1
buffer = ctypes.create_string_buffer(buffer_size)
smtc_dll.SMTC_GetTitle(buffer, buffer_size)
Decode using utf-8 to match C++'s strncpy_s output
return buffer.value.decode('utf-8', errors='ignore')

Example Call
smtc_dll.InitSMTC()

... wait 2 seconds for WinRT initialization
print(f"Current Title: {get_title()}") print(f"Is Playing: {smtc_dll.SMTC_GetPlaybackStatus()}") smtc_dll.SMTC_PlayPause()
```

## ⚠️ Important Notes
Resource Cleanup: You must call ShutdownSMTC() when your application or Mod exits to safely detach WinRT event listeners and release resources. Failure to do so can lead to program freezing or crashes due to dangling threads.

Thread Safety: The C++ internal implementation uses std::mutex to protect the cached media data (g_title, g_isPlaying, etc.), ensuring data safety between the asynchronous WinRT background threads and your main calling thread.

Character Set: The exported strings use char* and strncpy_s, which means they are output as ANSI/UTF-8 encoded strings. Ensure your consuming application (C# or Python) uses the corresponding encoding (e.g., decode('utf-8') in the Python example) for correct interpretation.