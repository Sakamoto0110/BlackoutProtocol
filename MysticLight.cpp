#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <oleauto.h>
#include <objbase.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "MysticLight.h"


// ============================================================================
// SDK signatures
//
// Transcribed from the official sample (Sample/MysticLight_Test(C++).cpp),
// with the corrections it needs:
//
//     - the sample loads MysticLight_SDK.dll, which is the x86 build; an
//       x64 process needs MysticLight_SDK_x64.dll
//
//     - the sample reads pDevType->rgsabound->cElements directly, assuming
//       a lower bound of zero; the bounds are actually queried here
//
// Everything is resolved through GetProcAddress, so a missing DLL stops
// neither the link nor the run.
// ============================================================================

namespace
{
    typedef int (*FnInitialize)();
    typedef int (*FnRelease)();
    typedef int (*FnGetDeviceInfo)(SAFEARRAY** devType, SAFEARRAY** ledCount);
    typedef int (*FnGetLedInfo)(BSTR type, DWORD index, BSTR* name, SAFEARRAY** styles);
    typedef int (*FnGetLedColor)(BSTR type, DWORD index, DWORD* r, DWORD* g, DWORD* b);
    typedef int (*FnGetLedStyle)(BSTR type, DWORD index, BSTR* style);
    typedef int (*FnGetLedBright)(BSTR type, DWORD index, DWORD* level);
    typedef int (*FnGetLedMaxBright)(BSTR type, DWORD index, DWORD* level);
    typedef int (*FnGetLedSpeed)(BSTR type, DWORD index, DWORD* level);
    typedef int (*FnSetLedColor)(BSTR type, DWORD index, DWORD r, DWORD g, DWORD b);
    typedef int (*FnSetLedStyle)(BSTR type, DWORD index, BSTR style);
    typedef int (*FnSetLedBright)(BSTR type, DWORD index, DWORD level);
    typedef int (*FnSetLedSpeed)(BSTR type, DWORD index, DWORD level);


    HMODULE g_lib = nullptr;

    FnInitialize      mlInitialize = nullptr;
    FnRelease         mlRelease = nullptr;
    FnGetDeviceInfo   mlGetDeviceInfo = nullptr;
    FnGetLedInfo      mlGetLedInfo = nullptr;
    FnGetLedColor     mlGetLedColor = nullptr;
    FnGetLedStyle     mlGetLedStyle = nullptr;
    FnGetLedBright    mlGetLedBright = nullptr;
    FnGetLedMaxBright mlGetLedMaxBright = nullptr;
    FnGetLedSpeed     mlGetLedSpeed = nullptr;
    FnSetLedColor     mlSetLedColor = nullptr;
    FnSetLedStyle     mlSetLedStyle = nullptr;
    FnSetLedBright    mlSetLedBright = nullptr;
    FnSetLedSpeed     mlSetLedSpeed = nullptr;


    // ========================================================================
    // Blackout style
    //
    // "Off" is enumerated by MLAPI_GetLedInfo on all four board zones
    // (PCH, JRGB1, JRAINBOW1, JRAINBOW2), and is the first entry in the list.
    // ========================================================================

    constexpr wchar_t STYLE_OFF[] = L"Off";


    // ========================================================================
    // Blackout indicator
    //
    // One zone is not blacked out with the rest. It is left flashing red at
    // half brightness for as long as the blackout lasts.
    //
    // This is the only visible sign that the program is holding the displays
    // down: it has no window and no tray icon.
    //
    // The zone is picked BY NAME, not by index -- enumeration order is not
    // contractual. "Flashing" is among the styles PCH reports as supported.
    //
    // Its original state is captured along with every other zone, so the
    // restore puts it back exactly as it was.
    // ========================================================================

    constexpr wchar_t INDICATOR_ZONE[] = L"PCH";
    constexpr wchar_t INDICATOR_STYLE[] = L"Flashing";

    constexpr DWORD INDICATOR_R = 255;
    constexpr DWORD INDICATOR_G = 0;
    constexpr DWORD INDICATOR_B = 0;


    // ========================================================================
    // Requests handed to the thread
    // ========================================================================

    constexpr UINT WM_ML_OFF = WM_APP + 1;
    constexpr UINT WM_ML_ON = WM_APP + 2;


    // ========================================================================
    // One LED zone
    // ========================================================================

    struct Zone
    {
        std::wstring type;      // "MSI_MB"
        DWORD        index = 0;

        // Discovered during enumeration, never persisted.
        //
        std::wstring name;      // "PCH", "JRGB1", "JRAINBOW1", "JRAINBOW2"
        DWORD        maxBright = 0;

        // State captured before the blackout.
        //
        std::wstring style;
        DWORD        r = 0;
        DWORD        g = 0;
        DWORD        b = 0;
        DWORD        bright = 0;
        DWORD        speed = 0;
    };


    std::vector<Zone> g_zones;

    bool g_sdkReady = false;

    HANDLE g_thread = nullptr;
    DWORD  g_threadId = 0;

    // Signalled by the thread as soon as its message queue exists.
    //
    // PostThreadMessage fails silently against a thread that has no queue
    // yet, which is why Start() waits on this.
    //
    HANDLE g_queueReady = nullptr;


    // Defined further down with the rest of the state file helpers.
    //
    std::wstring StateDirectory();


    // ========================================================================
    // Logging
    //
    // To disk, not just to the debugger.
    //
    // This program has no window and runs elevated, so attaching a debugger
    // is the only other way to see anything -- and by the time something has
    // gone wrong, the moment has passed. A few lines on disk are the
    // difference between reading what happened and guessing at it.
    //
    // UTF-8, appended, shared for reading, so the file can be tailed while
    // the program runs.
    // ========================================================================

    void Log(const wchar_t* msg)
    {
        OutputDebugStringW(L"[MysticLight] ");
        OutputDebugStringW(msg);
        OutputDebugStringW(L"\n");


        const std::wstring dir = StateDirectory();

        if (dir.empty())
            return;

        CreateDirectoryW(dir.c_str(), nullptr);


        SYSTEMTIME st{};

        GetLocalTime(&st);


        wchar_t line[1024];

        swprintf_s(
            line,
            L"%02u:%02u:%02u.%03u  %s\r\n",
            st.wHour,
            st.wMinute,
            st.wSecond,
            st.wMilliseconds,
            msg
        );


        const int bytes = WideCharToMultiByte(
            CP_UTF8,
            0,
            line,
            -1,
            nullptr,
            0,
            nullptr,
            nullptr
        );

        if (bytes <= 1)
            return;

        std::vector<char> utf8(static_cast<size_t>(bytes));

        WideCharToMultiByte(
            CP_UTF8,
            0,
            line,
            -1,
            utf8.data(),
            bytes,
            nullptr,
            nullptr
        );


        HANDLE h = CreateFileW(
            (dir + L"\\blackout.log").c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (h == INVALID_HANDLE_VALUE)
            return;


        DWORD written = 0;

        // bytes - 1 drops the terminating NUL.
        //
        WriteFile(
            h,
            utf8.data(),
            static_cast<DWORD>(bytes - 1),
            &written,
            nullptr
        );


        CloseHandle(h);
    }


    // Every MLAPI setter returns a status that was previously thrown away.
    // Silence is exactly what makes "it ran but nothing happened" so hard
    // to tell apart from "it never ran".
    //
    void LogRc(
        const wchar_t* what,
        DWORD zone,
        int rc)
    {
        wchar_t buf[256];

        swprintf_s(
            buf,
            L"%s zone %lu -> rc=%d%s",
            what,
            zone,
            rc,
            (rc == S_OK) ? L"" : L"   <<< FAILED"
        );

        Log(buf);
    }


    // ========================================================================
    // SAFEARRAY
    // ========================================================================

    LONG SaCount(SAFEARRAY* sa)
    {
        if (sa == nullptr)
            return 0;

        LONG lo = 0;
        LONG hi = 0;

        if (FAILED(SafeArrayGetLBound(sa, 1, &lo)))
            return 0;

        if (FAILED(SafeArrayGetUBound(sa, 1, &hi)))
            return 0;

        return hi - lo + 1;
    }


    BSTR SaAt(SAFEARRAY* sa, LONG i)
    {
        if (sa == nullptr)
            return nullptr;

        LONG lo = 0;

        if (FAILED(SafeArrayGetLBound(sa, 1, &lo)))
            return nullptr;

        LONG idx = lo + i;
        BSTR out = nullptr;

        if (FAILED(SafeArrayGetElement(sa, &idx, &out)))
            return nullptr;

        return out;
    }


    // ========================================================================
    // Loading the DLL
    //
    // ALWAYS by absolute path.
    //
    // This process runs elevated. LoadLibraryW with a bare name would
    // consult the working directory and PATH, which in a process holding
    // administrator privilege is a DLL hijacking vector.
    //
    // Search order:
    //
    //     1. next to the executable   (self-contained install)
    //     2. the known SDK location
    //
    // The SDK ships one DLL per architecture and a process can only load its
    // own. Picking the wrong one fails silently -- the load simply never
    // succeeds and the lighting is left alone forever -- so the name is tied
    // to the build rather than written out once.
    // ========================================================================

#ifdef _WIN64
    constexpr wchar_t SDK_DLL[] = L"MysticLight_SDK_x64.dll";
#else
    constexpr wchar_t SDK_DLL[] = L"MysticLight_SDK.dll";
#endif

    constexpr wchar_t SDK_FALLBACK_DIR[] =
        L"C:\\Mystic light SDK\\Mystic_light_SDK_1.0.0.08\\";

    std::wstring ExeDirectory()
    {
        wchar_t path[MAX_PATH] = {};

        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);

        if (n == 0 || n >= MAX_PATH)
            return std::wstring();

        std::wstring s(path, n);

        const size_t cut = s.find_last_of(L'\\');

        if (cut == std::wstring::npos)
            return std::wstring();

        return s.substr(0, cut + 1);
    }


    bool LoadSdk()
    {
        const std::wstring exeDir = ExeDirectory();

        std::vector<std::wstring> candidates;

        if (!exeDir.empty())
            candidates.push_back(exeDir + SDK_DLL);

        candidates.push_back(std::wstring(SDK_FALLBACK_DIR) + SDK_DLL);

        for (const std::wstring& c : candidates)
        {
            g_lib = LoadLibraryW(c.c_str());

            if (g_lib != nullptr)
            {
                Log(c.c_str());
                break;
            }
        }

        if (g_lib == nullptr)
        {
            Log(L"SDK not found");
            return false;
        }

        mlInitialize      = reinterpret_cast<FnInitialize>     (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_Initialize")));
        mlRelease         = reinterpret_cast<FnRelease>        (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_Release")));
        mlGetDeviceInfo   = reinterpret_cast<FnGetDeviceInfo>  (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_GetDeviceInfo")));
        mlGetLedInfo      = reinterpret_cast<FnGetLedInfo>     (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_GetLedInfo")));
        mlGetLedColor     = reinterpret_cast<FnGetLedColor>    (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_GetLedColor")));
        mlGetLedStyle     = reinterpret_cast<FnGetLedStyle>    (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_GetLedStyle")));
        mlGetLedBright    = reinterpret_cast<FnGetLedBright>   (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_GetLedBright")));
        mlGetLedMaxBright = reinterpret_cast<FnGetLedMaxBright>(reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_GetLedMaxBright")));
        mlGetLedSpeed     = reinterpret_cast<FnGetLedSpeed>    (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_GetLedSpeed")));
        mlSetLedColor     = reinterpret_cast<FnSetLedColor>    (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_SetLedColor")));
        mlSetLedStyle     = reinterpret_cast<FnSetLedStyle>    (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_SetLedStyle")));
        mlSetLedBright    = reinterpret_cast<FnSetLedBright>   (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_SetLedBright")));
        mlSetLedSpeed     = reinterpret_cast<FnSetLedSpeed>    (reinterpret_cast<void*>(GetProcAddress(g_lib, "MLAPI_SetLedSpeed")));

        if (mlInitialize == nullptr ||
            mlGetDeviceInfo == nullptr ||
            mlSetLedStyle == nullptr)
        {
            Log(L"essential exports missing");

            FreeLibrary(g_lib);
            g_lib = nullptr;

            return false;
        }

        return true;
    }


    // ========================================================================
    // Zone enumeration
    //
    // Done once. The set of zones on the board does not change at runtime.
    // ========================================================================

    bool EnumerateZones()
    {
        SAFEARRAY* types = nullptr;
        SAFEARRAY* counts = nullptr;

        if (mlGetDeviceInfo(&types, &counts) != S_OK)
        {
            Log(L"GetDeviceInfo failed -- no elevation, or MSI Center is down");
            return false;
        }

        const LONG n = SaCount(types);

        for (LONG d = 0; d < n; d++)
        {
            BSTR type = SaAt(types, d);
            BSTR cnt = SaAt(counts, d);

            if (type != nullptr)
            {
                const int zones = (cnt != nullptr) ? _wtoi(cnt) : 0;

                for (int i = 0; i < zones; i++)
                {
                    Zone z;

                    z.type = type;
                    z.index = static_cast<DWORD>(i);

                    // Zone name. This is how the indicator zone is picked,
                    // rather than by a hardcoded index.
                    //
                    if (mlGetLedInfo != nullptr)
                    {
                        BSTR ledName = nullptr;
                        SAFEARRAY* styles = nullptr;

                        if (mlGetLedInfo(type, z.index, &ledName, &styles) == S_OK)
                        {
                            if (ledName != nullptr)
                            {
                                z.name = ledName;
                                SysFreeString(ledName);
                            }

                            if (styles != nullptr)
                                SafeArrayDestroy(styles);
                        }
                    }

                    // Brightness ceiling, read from the board rather than
                    // assumed.
                    //
                    if (mlGetLedMaxBright != nullptr)
                        mlGetLedMaxBright(type, z.index, &z.maxBright);

                    g_zones.push_back(z);
                }

                SysFreeString(type);
            }

            if (cnt != nullptr)
                SysFreeString(cnt);
        }

        if (types != nullptr)
            SafeArrayDestroy(types);

        if (counts != nullptr)
            SafeArrayDestroy(counts);

        return !g_zones.empty();
    }


    // ========================================================================
    // State file
    //
    // %LOCALAPPDATA%\BlackoutProtocol\ledstate.dat
    //
    // The EXISTENCE of this file is what means "the lights are off because
    // of us". It is written BEFORE anything is blacked out, so a crash
    // mid-operation still leaves recoverable state, and deleted only after
    // a successful restore.
    //
    // Stored as raw UTF-16LE with a BOM: that is what wchar_t already is, so
    // no encoding conversion happens anywhere.
    //
    // Format, one zone per line:
    //
    //     type|index|style|r|g|b|brightness|speed
    // ========================================================================

    std::wstring StateDirectory()
    {
        wchar_t buf[MAX_PATH] = {};

        const DWORD n =
            GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);

        if (n == 0 || n >= MAX_PATH)
            return std::wstring();

        return std::wstring(buf) + L"\\BlackoutProtocol";
    }


    std::wstring StateFilePath()
    {
        const std::wstring dir = StateDirectory();

        if (dir.empty())
            return std::wstring();

        return dir + L"\\ledstate.dat";
    }


    bool StateFileExists()
    {
        const std::wstring p = StateFilePath();

        if (p.empty())
            return false;

        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    }


    bool WriteStateFile()
    {
        const std::wstring dir = StateDirectory();
        const std::wstring path = StateFilePath();

        if (path.empty())
            return false;

        CreateDirectoryW(dir.c_str(), nullptr);

        std::wstring text;

        text += L'\xFEFF';

        for (const Zone& z : g_zones)
        {
            text += z.type;
            text += L'|';
            text += std::to_wstring(z.index);
            text += L'|';
            text += z.style;
            text += L'|';
            text += std::to_wstring(z.r);
            text += L'|';
            text += std::to_wstring(z.g);
            text += L'|';
            text += std::to_wstring(z.b);
            text += L'|';
            text += std::to_wstring(z.bright);
            text += L'|';
            text += std::to_wstring(z.speed);
            text += L"\r\n";
        }

        HANDLE h = CreateFileW(
            path.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (h == INVALID_HANDLE_VALUE)
        {
            Log(L"could not write the state file");
            return false;
        }

        const DWORD bytes =
            static_cast<DWORD>(text.size() * sizeof(wchar_t));

        DWORD written = 0;

        const BOOL ok =
            WriteFile(h, text.data(), bytes, &written, nullptr);

        // Make sure the state reached the disk before anything goes dark.
        //
        FlushFileBuffers(h);

        CloseHandle(h);

        return ok && written == bytes;
    }


    bool ReadStateFile(std::vector<Zone>& out)
    {
        const std::wstring path = StateFilePath();

        if (path.empty())
            return false;

        HANDLE h = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (h == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size{};

        if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20))
        {
            CloseHandle(h);
            return false;
        }

        std::wstring text;

        text.resize(static_cast<size_t>(size.QuadPart) / sizeof(wchar_t));

        DWORD read = 0;

        const BOOL ok = ReadFile(
            h,
            text.data(),
            static_cast<DWORD>(text.size() * sizeof(wchar_t)),
            &read,
            nullptr
        );

        CloseHandle(h);

        if (!ok)
            return false;

        if (!text.empty() && text.front() == L'\xFEFF')
            text.erase(text.begin());

        size_t pos = 0;

        while (pos < text.size())
        {
            size_t eol = text.find(L"\r\n", pos);

            if (eol == std::wstring::npos)
                eol = text.size();

            const std::wstring line = text.substr(pos, eol - pos);

            pos = eol + 2;

            if (line.empty())
                continue;

            std::vector<std::wstring> f;

            size_t fp = 0;

            while (true)
            {
                const size_t bar = line.find(L'|', fp);

                if (bar == std::wstring::npos)
                {
                    f.push_back(line.substr(fp));
                    break;
                }

                f.push_back(line.substr(fp, bar - fp));

                fp = bar + 1;
            }

            if (f.size() != 8)
                continue;

            Zone z;

            z.type   = f[0];
            z.index  = static_cast<DWORD>(_wtoi(f[1].c_str()));
            z.style  = f[2];
            z.r      = static_cast<DWORD>(_wtoi(f[3].c_str()));
            z.g      = static_cast<DWORD>(_wtoi(f[4].c_str()));
            z.b      = static_cast<DWORD>(_wtoi(f[5].c_str()));
            z.bright = static_cast<DWORD>(_wtoi(f[6].c_str()));
            z.speed  = static_cast<DWORD>(_wtoi(f[7].c_str()));

            out.push_back(z);
        }

        return !out.empty();
    }


    void DeleteStateFile()
    {
        const std::wstring path = StateFilePath();

        if (!path.empty())
            DeleteFileW(path.c_str());
    }


    // ========================================================================
    // Capture and apply
    // ========================================================================

    void CaptureCurrentState()
    {
        for (Zone& z : g_zones)
        {
            BSTR type = SysAllocString(z.type.c_str());

            if (type == nullptr)
                continue;

            BSTR style = nullptr;

            if (mlGetLedStyle != nullptr &&
                mlGetLedStyle(type, z.index, &style) == S_OK &&
                style != nullptr)
            {
                z.style = style;
                SysFreeString(style);
            }

            if (mlGetLedColor != nullptr)
                mlGetLedColor(type, z.index, &z.r, &z.g, &z.b);

            if (mlGetLedBright != nullptr)
                mlGetLedBright(type, z.index, &z.bright);

            if (mlGetLedSpeed != nullptr)
                mlGetLedSpeed(type, z.index, &z.speed);

            SysFreeString(type);
        }
    }


    void ApplyIndicator(const Zone& z)
    {
        BSTR type = SysAllocString(z.type.c_str());

        if (type == nullptr)
            return;

        if (mlSetLedColor != nullptr)
        {
            LogRc(
                L"indicator SetLedColor",
                z.index,
                mlSetLedColor(type, z.index, INDICATOR_R, INDICATOR_G, INDICATOR_B)
            );
        }

        BSTR style = SysAllocString(INDICATOR_STYLE);

        if (style != nullptr)
        {
            LogRc(
                L"indicator SetLedStyle(Flashing)",
                z.index,
                mlSetLedStyle(type, z.index, style)
            );

            SysFreeString(style);
        }

        // Brightness after the style: changing style can reset it.
        //
        // This board's scale is 0..5, so "half" lands between two levels.
        // Rounds down, with a floor of 1 -- an indicator at zero brightness
        // would not indicate anything.
        //
        if (mlSetLedBright != nullptr && z.maxBright > 0)
        {
            DWORD level = z.maxBright / 2;

            if (level == 0)
                level = 1;

            mlSetLedBright(type, z.index, level);
        }

        SysFreeString(type);
    }


    void ApplyOff()
    {
        for (const Zone& z : g_zones)
        {
            // The indicator zone does not go dark. It flashes for as long as
            // the blackout lasts. Its original state was captured along with
            // every other zone, so the restore puts it back.
            //
            if (z.name == INDICATOR_ZONE)
            {
                ApplyIndicator(z);
                continue;
            }

            BSTR type = SysAllocString(z.type.c_str());
            BSTR style = SysAllocString(STYLE_OFF);

            if (type != nullptr && style != nullptr)
            {
                LogRc(
                    L"SetLedStyle(Off)",
                    z.index,
                    mlSetLedStyle(type, z.index, style)
                );
            }

            if (style != nullptr)
                SysFreeString(style);

            if (type != nullptr)
                SysFreeString(type);
        }
    }


    void ApplyZone(const Zone& z)
    {
        BSTR type = SysAllocString(z.type.c_str());

        if (type == nullptr)
            return;

        // Color before style, as in MSI's own sample.
        //
        if (mlSetLedColor != nullptr)
        {
            LogRc(
                L"restore SetLedColor",
                z.index,
                mlSetLedColor(type, z.index, z.r, z.g, z.b)
            );
        }

        if (!z.style.empty())
        {
            BSTR style = SysAllocString(z.style.c_str());

            if (style != nullptr)
            {
                LogRc(
                    L"restore SetLedStyle",
                    z.index,
                    mlSetLedStyle(type, z.index, style)
                );

                SysFreeString(style);
            }
        }

        // Brightness and speed after the style: switching style can reset
        // both of them.
        //
        if (mlSetLedBright != nullptr)
            mlSetLedBright(type, z.index, z.bright);

        if (mlSetLedSpeed != nullptr)
            mlSetLedSpeed(type, z.index, z.speed);

        SysFreeString(type);
    }


    // ========================================================================
    // Operations, already on the thread
    // ========================================================================

    void DoLightsOff()
    {
        if (!g_sdkReady)
            return;

        // State is already saved.
        //
        // Capturing again would record the blacked-out state itself as the
        // state to restore, losing the original for good.
        //
        if (StateFileExists())
        {
            Log(L"state already saved -- just reapplying the blackout");
            ApplyOff();
            return;
        }

        CaptureCurrentState();

        if (!WriteStateFile())
        {
            // With no state on disk there is no way to guarantee a restore.
            // Better to leave the lights on than to turn them off for good.
            //
            Log(L"aborted: state could not be written");
            return;
        }

        ApplyOff();

        Log(L"lights out");
    }


    void DoLightsOn()
    {
        if (!g_sdkReady)
            return;

        std::vector<Zone> saved;

        if (!ReadStateFile(saved))
        {
            Log(L"nothing saved -- nothing to restore");
            return;
        }

        for (const Zone& z : saved)
            ApplyZone(z);

        DeleteStateFile();

        Log(L"lights restored");
    }


    // ========================================================================
    // The thread
    // ========================================================================

    DWORD WINAPI ThreadProc(LPVOID)
    {
        // Force the message queue into existence before Start() is released.
        //
        MSG probe{};

        PeekMessageW(&probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        SetEvent(g_queueReady);


        // MULTITHREADED, and the choice is load-bearing.
        //
        // This thread blocks inside MLAPI calls and never pumps messages
        // while it does. In a single-threaded apartment that is a deadlock
        // waiting to happen: any COM call the SDK marshals back into the
        // caller's apartment has nobody to dispatch it.
        //
        // It shows up asymmetrically, which is what makes it confusing.
        // Reads -- GetDeviceInfo, GetLedStyle, GetLedColor -- all return
        // fine under STA. The setters are what hang, leaving the thread
        // asleep forever with the state file already written and the queued
        // restore never processed.
        //
        // An MTA thread needs no pump: COM dispatches incoming calls on its
        // own RPC worker threads.
        //
        const HRESULT co =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);


        if (mlInitialize() == S_OK && EnumerateZones())
        {
            g_sdkReady = true;

            Log(L"SDK ready");

            // Recovery.
            //
            // A state file present at startup means the previous run died
            // with the blackout active: the lights are off and nobody
            // restored them.
            //
            if (StateFileExists())
            {
                Log(L"orphaned state found -- restoring");
                DoLightsOn();
            }
        }
        else
        {
            Log(L"SDK unavailable -- lighting will not be controlled");
        }


        MSG msg{};

        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            switch (msg.message)
            {
            case WM_ML_OFF:
                DoLightsOff();
                break;

            case WM_ML_ON:
                DoLightsOn();
                break;

            default:
                break;
            }
        }


        if (g_sdkReady && mlRelease != nullptr)
            mlRelease();

        if (SUCCEEDED(co))
            CoUninitialize();

        return 0;
    }
}


// ============================================================================
// Public interface
// ============================================================================

namespace MysticLight
{
    bool Start()
    {
        if (g_thread != nullptr)
            return g_sdkReady;

        if (!LoadSdk())
            return false;

        g_queueReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (g_queueReady == nullptr)
        {
            FreeLibrary(g_lib);
            g_lib = nullptr;

            return false;
        }

        g_thread = CreateThread(
            nullptr,
            0,
            ThreadProc,
            nullptr,
            0,
            &g_threadId
        );

        if (g_thread == nullptr)
        {
            CloseHandle(g_queueReady);
            g_queueReady = nullptr;

            FreeLibrary(g_lib);
            g_lib = nullptr;

            return false;
        }

        // PostThreadMessage fails against a thread with no queue yet.
        //
        WaitForSingleObject(g_queueReady, 5000);

        return true;
    }


    void Stop()
    {
        if (g_thread == nullptr)
            return;

        // A clean exit restores the lighting.
        //
        PostThreadMessageW(g_threadId, WM_ML_ON, 0, 0);


        // WM_QUIT is only picked up after the messages already sitting in
        // the queue, so the restore above happens first.
        //
        PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);


        // The thread may be in the middle of an MLAPI call, which is slow.
        //
        const DWORD wait = WaitForSingleObject(g_thread, 10000);

        CloseHandle(g_thread);
        g_thread = nullptr;
        g_threadId = 0;

        if (g_queueReady != nullptr)
        {
            CloseHandle(g_queueReady);
            g_queueReady = nullptr;
        }

        // Only unload once the thread is provably gone.
        //
        // On a timeout it may still be executing inside the DLL, and
        // unloading code out from under a running thread is a crash. The
        // leak is bounded: this only ever runs on the way out, and the
        // process is about to take the module with it anyway.
        //
        if (wait == WAIT_OBJECT_0 && g_lib != nullptr)
        {
            FreeLibrary(g_lib);
            g_lib = nullptr;
        }
        else if (wait != WAIT_OBJECT_0)
        {
            Log(L"thread did not exit in time -- leaving the SDK loaded");
        }

        g_sdkReady = false;
    }


    void LightsOff()
    {
        if (g_threadId != 0)
            PostThreadMessageW(g_threadId, WM_ML_OFF, 0, 0);
    }


    void LightsOn()
    {
        if (g_threadId != 0)
            PostThreadMessageW(g_threadId, WM_ML_ON, 0, 0);
    }
}
