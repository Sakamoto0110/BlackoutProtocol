#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0602

#include <windows.h>
#include <shellapi.h>
#include <cstring>

#include "resource.h"


// ============================================================================
// Configuration
// ============================================================================

constexpr int HOTKEY_BLACKOUT = 1;

// Ctrl + Shift + Alt + F11
constexpr UINT HOTKEY_MODIFIERS =
MOD_CONTROL |
MOD_SHIFT |
MOD_ALT |
MOD_NOREPEAT;

constexpr UINT HOTKEY_KEY = VK_F11;


// Custom message used to defer monitor shutdown.
constexpr UINT WM_BLACKOUT_FORCE_OFF = WM_APP + 1;


// Notification area callback.
//
// The shell posts icon events here. WM_APP + 1 is already taken above, so
// anything added later must keep counting up.
constexpr UINT WM_TRAYICON = WM_APP + 2;


constexpr UINT TRAY_ICON_ID = 1;

constexpr UINT IDM_TRAY_EXIT = 1;


// One-shot-ish wake confirmation timer.
//
// It is explicitly killed after firing, so there is no periodic activity.
constexpr UINT_PTR TIMER_WAKE_CONFIRM = 1;

constexpr UINT WAKE_CONFIRM_DELAY_MS = 150;


// ============================================================================
// GUID_CONSOLE_DISPLAY_STATE
//
// Notification data:
//
//   0 = display off
//   1 = display on
//   2 = display dimmed
//
// Defined locally to avoid depending on additional SDK GUID linkage.
// ============================================================================

static const GUID GUID_CONSOLE_DISPLAY_STATE_LOCAL =
{
    0x6fe69556,
    0x704a,
    0x47a0,
    {
        0x8f,
        0x24,
        0xc2,
        0x8d,
        0x93,
        0x6f,
        0xda,
        0x47
    }
};


// ============================================================================
// Global state
// ============================================================================

static HWND g_hwnd = nullptr;

static HPOWERNOTIFY g_powerNotify = nullptr;

static HANDLE g_mutex = nullptr;

static bool g_blackout = false;


// Notification area.
//
// The icon handle is kept because the icon has to be recreated from scratch
// if Explorer restarts.
static HICON g_trayIcon = nullptr;

static bool g_trayAdded = false;

static UINT g_taskbarCreated = 0;


// ============================================================================
// Monitor power
// ============================================================================

static void SetMonitorPower(LPARAM state)
{
    DWORD_PTR result = 0;

    SendMessageTimeoutW(
        HWND_BROADCAST,
        WM_SYSCOMMAND,
        SC_MONITORPOWER,
        state,

        SMTO_ABORTIFHUNG |
        SMTO_BLOCK,

        1000,
        &result
    );
}


static void TurnMonitorsOff()
{
    // SC_MONITORPOWER:
    //
    // 2 = power off display
    //
    SetMonitorPower(static_cast<LPARAM>(2));
}


static void TurnMonitorsOn()
{
    // SC_MONITORPOWER:
    //
    // -1 = power on display
    //
    SetMonitorPower(static_cast<LPARAM>(-1));
}


// ============================================================================
// Wake sequence
// ============================================================================

static void WakeMonitors()
{
    // Tell Windows that the display is currently required.
    //
    // Without ES_CONTINUOUS this does NOT permanently disable the configured
    // display timeout. It merely resets the display idle state.
    //
    SetThreadExecutionState(
        ES_DISPLAY_REQUIRED
    );


    // Explicit display power-on request.
    //
    TurnMonitorsOn();
}


// ============================================================================
// Pending message cleanup
// ============================================================================

static void RemovePendingForceOffMessages()
{
    MSG pending{};

    while (
        PeekMessageW(
            &pending,
            g_hwnd,

            WM_BLACKOUT_FORCE_OFF,
            WM_BLACKOUT_FORCE_OFF,

            PM_REMOVE
        )
        )
    {
        // Intentionally empty.
        //
        // We only want to discard already queued force-off operations.
    }
}


// ============================================================================
// Blackout state
// ============================================================================

static void EnableBlackout()
{
    if (g_blackout)
        return;


    // If the user toggles blackout back ON during the 150 ms wake window,
    // cancel the pending wake confirmation first.
    //
    KillTimer(
        g_hwnd,
        TIMER_WAKE_CONFIRM
    );


    g_blackout = true;


    TurnMonitorsOff();
}


static void DisableBlackout()
{
    if (!g_blackout)
        return;


    // IMPORTANT:
    //
    // Disable the guard BEFORE doing anything capable of waking the displays.
    //
    // Otherwise our own display-state notification could be interpreted as
    // an unwanted wake and immediately shut the monitors down again.
    //
    g_blackout = false;


    // A display-on event may already have caused WM_BLACKOUT_FORCE_OFF to be
    // queued before WM_HOTKEY was processed.
    //
    // Remove any stale shutdown requests.
    //
    RemovePendingForceOffMessages();


    // Initial wake.
    //
    WakeMonitors();


    // Some GPU / HDMI / DisplayPort combinations can briefly transition
    // through ON/OFF states while the link wakes.
    //
    // Confirm the wake once after a very short delay.
    //
    // SetTimer replaces an existing timer with the same ID, so this cannot
    // accumulate multiple timers.
    //
    SetTimer(
        g_hwnd,
        TIMER_WAKE_CONFIRM,
        WAKE_CONFIRM_DELAY_MS,
        nullptr
    );
}


static void ToggleBlackout()
{
    if (g_blackout)
    {
        DisableBlackout();
    }
    else
    {
        EnableBlackout();
    }
}


// ============================================================================
// Notification area
//
// The only piece of UI the program has. It exists for one reason: to offer a
// way out that is not Task Manager.
//
// Exiting through the menu goes through WM_CLOSE, so the window is destroyed
// in the ordinary way and the cleanup at the end of wWinMain runs: the hotkey
// is unregistered, the power notification handle is released, and the icon is
// taken out of the notification area instead of being left behind as a ghost.
// ============================================================================

static bool AddTrayIcon(
    HWND hwnd,
    HICON icon)
{
    g_trayIcon = icon;


    NOTIFYICONDATAW nid{};

    nid.cbSize = sizeof(nid);

    nid.hWnd = hwnd;

    nid.uID = TRAY_ICON_ID;

    nid.uFlags =
        NIF_ICON |
        NIF_MESSAGE |
        NIF_TIP |
        NIF_SHOWTIP;

    nid.uCallbackMessage = WM_TRAYICON;

    nid.hIcon = icon;


    wcscpy_s(
        nid.szTip,
        L"Blackout Protocol"
    );


    if (!Shell_NotifyIconW(NIM_ADD, &nid))
    {
        return false;
    }


    // NIF_SHOWTIP and the packed coordinates in the callback both require
    // version 4. Without this the tooltip never appears.
    //
    nid.uVersion = NOTIFYICON_VERSION_4;

    Shell_NotifyIconW(
        NIM_SETVERSION,
        &nid
    );


    g_trayAdded = true;

    return true;
}


static void RemoveTrayIcon(
    HWND hwnd)
{
    if (!g_trayAdded)
        return;


    NOTIFYICONDATAW nid{};

    nid.cbSize = sizeof(nid);

    nid.hWnd = hwnd;

    nid.uID = TRAY_ICON_ID;


    Shell_NotifyIconW(
        NIM_DELETE,
        &nid
    );


    g_trayAdded = false;
}


static void ShowTrayMenu(
    HWND hwnd,
    int x,
    int y)
{
    HMENU menu = CreatePopupMenu();

    if (menu == nullptr)
        return;


    AppendMenuW(
        menu,
        MF_STRING,
        IDM_TRAY_EXIT,
        L"Exit"
    );


    // Two documented requirements for tray menus, and both are easy to miss:
    //
    //     SetForegroundWindow before, or the menu will not dismiss when the
    //     user clicks somewhere else.
    //
    //     A posted message after, or the next activation is swallowed.
    //
    SetForegroundWindow(hwnd);


    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON,
        x,
        y,
        0,
        hwnd,
        nullptr
    );


    PostMessageW(
        hwnd,
        WM_NULL,
        0,
        0
    );


    DestroyMenu(menu);
}


// ============================================================================
// Window procedure
// ============================================================================

static LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    // Explorer restarting wipes every notification icon. The shell announces
    // it with a registered message, whose value is only known at runtime and
    // so cannot be a switch label.
    //
    if (message == g_taskbarCreated &&
        g_taskbarCreated != 0)
    {
        g_trayAdded = false;

        AddTrayIcon(
            hwnd,
            g_trayIcon
        );

        return 0;
    }


    switch (message)
    {
        // ====================================================================
        // Global hotkey
        //
        // Ctrl + Shift + Alt + F11
        // ====================================================================

    case WM_HOTKEY:
    {
        if (wParam == HOTKEY_BLACKOUT)
        {
            ToggleBlackout();
        }

        return 0;
    }


    // ====================================================================
    // Deferred blackout enforcement
    // ====================================================================

    case WM_BLACKOUT_FORCE_OFF:
    {
        if (g_blackout)
        {
            TurnMonitorsOff();
        }

        return 0;
    }


    // ====================================================================
    // Wake confirmation
    // ====================================================================

    case WM_TIMER:
    {
        if (wParam == TIMER_WAKE_CONFIRM)
        {
            // Stop immediately.
            //
            // This timer exists only for this one wake confirmation.
            //
            KillTimer(
                hwnd,
                TIMER_WAKE_CONFIRM
            );


            // The user might have enabled blackout again during the
            // 150 ms interval.
            //
            // Never wake if blackout is currently active.
            //
            if (!g_blackout)
            {
                WakeMonitors();
            }


            return 0;
        }

        break;
    }


    // ====================================================================
    // Display power-state notifications
    // ====================================================================

    case WM_POWERBROADCAST:
    {
        if (wParam != PBT_POWERSETTINGCHANGE)
        {
            break;
        }


        const auto* setting =
            reinterpret_cast<const POWERBROADCAST_SETTING*>(
                lParam
                );


        if (setting == nullptr)
        {
            return TRUE;
        }


        // Ignore unrelated power settings.
        //
        if (!IsEqualGUID(
            setting->PowerSetting,
            GUID_CONSOLE_DISPLAY_STATE_LOCAL))
        {
            return TRUE;
        }


        // We expect a DWORD.
        //
        if (setting->DataLength < sizeof(DWORD))
        {
            return TRUE;
        }


        DWORD displayState = 0;


        std::memcpy(
            &displayState,
            setting->Data,
            sizeof(displayState)
        );


        // ---------------------------------------------------------------
        // displayState
        //
        // 0 = OFF
        // 1 = ON
        // 2 = DIMMED
        // ---------------------------------------------------------------
        //
        // During blackout:
        //
        // OFF:
        //     good, do nothing.
        //
        // ON / DIMMED:
        //     Windows, an application, driver or input event attempted
        //     to wake the display.
        //
        //     Queue another shutdown.
        //
        // We PostMessage instead of performing the operation directly
        // inside WM_POWERBROADCAST to avoid unnecessary reentrancy.
        //
        if (
            g_blackout &&
            displayState != 0
            )
        {
            PostMessageW(
                hwnd,
                WM_BLACKOUT_FORCE_OFF,
                0,
                0
            );
        }


        return TRUE;
    }


    // ====================================================================
    // Notification area icon
    // ====================================================================

    case WM_TRAYICON:
    {
        // Version 4 packs the event in the low word of lParam and the
        // screen coordinates in wParam. WM_CONTEXTMENU covers both the
        // right click and the keyboard menu key.
        //
        if (LOWORD(lParam) == WM_CONTEXTMENU)
        {
            ShowTrayMenu(
                hwnd,
                static_cast<short>(LOWORD(wParam)),
                static_cast<short>(HIWORD(wParam))
            );
        }

        return 0;
    }


    case WM_COMMAND:
    {
        if (LOWORD(wParam) == IDM_TRAY_EXIT)
        {
            // Through WM_CLOSE rather than straight to DestroyWindow, so
            // the shutdown path stays the same one the rest of the
            // program uses.
            //
            PostMessageW(
                hwnd,
                WM_CLOSE,
                0,
                0
            );

            return 0;
        }

        break;
    }


    // ====================================================================
    // Shutdown
    // ====================================================================

    case WM_CLOSE:
    {
        DestroyWindow(hwnd);

        return 0;
    }


    case WM_DESTROY:
    {
        // Before the window goes away, not in the cleanup block at the end
        // of wWinMain. By then this handle is stale, and the shell
        // identifies the icon by window plus id.
        //
        RemoveTrayIcon(hwnd);


        PostQuitMessage(0);

        return 0;
    }
    }


    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam
    );
}


// ============================================================================
// Application icon
//
// The icon is compiled into the executable by BlackoutProtocol.rc, so there
// is no file to find at runtime and no failure mode tied to the working
// directory.
//
// LR_SHARED:
//
//     The handle is cached and owned by the module. It stays valid for as
//     long as the module is loaded, and must NOT be passed to DestroyIcon.
//
//     That is exactly the lifetime a window class icon wants, which is why
//     nothing below appears in the cleanup section at the end of wWinMain.
//
// Asking for an explicit size lets Windows pick the matching frame out of
// the multi-resolution .ico (16/32/48/64/128/256) instead of rescaling one
// bitmap into every slot.
//
// On failure this returns nullptr, and Windows falls back to the default
// application icon. Nothing else in the program depends on it, so there is
// no error path worth taking.
// ============================================================================

static HICON LoadAppIcon(
    HINSTANCE instance,
    int width,
    int height)
{
    return static_cast<HICON>(
        LoadImageW(
            instance,

            MAKEINTRESOURCEW(IDI_APPICON),

            IMAGE_ICON,

            width,
            height,

            LR_DEFAULTCOLOR |
            LR_SHARED
        )
        );
}


// ============================================================================
// Entry point
// ============================================================================

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    // ========================================================================
    // Single-instance protection
    // ========================================================================

    g_mutex = CreateMutexW(
        nullptr,
        FALSE,
        L"Local\\NekoBlackoutMonitorGuard"
    );


    if (g_mutex == nullptr)
    {
        return 1;
    }


    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(
            g_mutex
        );

        g_mutex = nullptr;

        return 0;
    }


    // ========================================================================
    // Window class
    // ========================================================================

    constexpr wchar_t WINDOW_CLASS[] =
        L"NekoBlackoutMonitorGuardWindow";


    WNDCLASSEXW wc{};

    wc.cbSize = sizeof(wc);

    wc.hInstance = instance;

    wc.lpfnWndProc = WindowProc;

    wc.lpszClassName = WINDOW_CLASS;


    // Large icon.
    //
    // Alt + Tab, the window list, and anything asking the window for its
    // icon rather than reading the .exe.
    //
    wc.hIcon = LoadAppIcon(
        instance,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON)
    );


    // Small icon.
    //
    // Title bar and taskbar sizes. Loaded separately so the 16 px frame is
    // used as drawn instead of being downscaled from the 32 px one, which
    // matters for a mark this thin.
    //
    wc.hIconSm = LoadAppIcon(
        instance,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON)
    );


    if (!RegisterClassExW(&wc))
    {
        CloseHandle(
            g_mutex
        );

        g_mutex = nullptr;

        return 2;
    }


    // ========================================================================
    // Hidden window
    //
    // Never shown.
    //
    // Exists solely to receive:
    //
    //   WM_HOTKEY
    //   WM_POWERBROADCAST
    //   WM_TIMER
    //   WM_APP + ...
    // ========================================================================

    g_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,

        WINDOW_CLASS,

        L"Neko Blackout",

        0,

        0,
        0,
        0,
        0,

        nullptr,
        nullptr,
        instance,
        nullptr
    );


    if (g_hwnd == nullptr)
    {
        UnregisterClassW(
            WINDOW_CLASS,
            instance
        );


        CloseHandle(
            g_mutex
        );

        g_mutex = nullptr;


        return 3;
    }


    // ========================================================================
    // Global hotkey
    //
    // Ctrl + Shift + Alt + F11
    // ========================================================================

    if (!RegisterHotKey(
        g_hwnd,
        HOTKEY_BLACKOUT,
        HOTKEY_MODIFIERS,
        HOTKEY_KEY))
    {
        MessageBoxW(
            nullptr,

            L"Could not register Ctrl + Shift + Alt + F11.\n\n"
            L"Another application may already be using this hotkey.",

            L"Blackout",

            MB_OK |
            MB_ICONERROR
        );


        DestroyWindow(
            g_hwnd
        );

        g_hwnd = nullptr;


        UnregisterClassW(
            WINDOW_CLASS,
            instance
        );


        CloseHandle(
            g_mutex
        );

        g_mutex = nullptr;


        return 4;
    }


    // ========================================================================
    // Display power notifications
    // ========================================================================

    g_powerNotify =
        RegisterPowerSettingNotification(
            g_hwnd,
            &GUID_CONSOLE_DISPLAY_STATE_LOCAL,
            DEVICE_NOTIFY_WINDOW_HANDLE
        );


    if (g_powerNotify == nullptr)
    {
        MessageBoxW(
            nullptr,

            L"Could not register display power notifications.",

            L"Blackout",

            MB_OK |
            MB_ICONERROR
        );


        UnregisterHotKey(
            g_hwnd,
            HOTKEY_BLACKOUT
        );


        DestroyWindow(
            g_hwnd
        );

        g_hwnd = nullptr;


        UnregisterClassW(
            WINDOW_CLASS,
            instance
        );


        CloseHandle(
            g_mutex
        );

        g_mutex = nullptr;


        return 5;
    }


    // ========================================================================
    // Notification area
    //
    // The registered message is looked up before the icon is added, because
    // Explorer could restart at any moment and WindowProc has to recognise
    // the announcement by the time it does.
    //
    // Failing here is not fatal. The hotkey still works; there is just no
    // menu to quit from.
    // ========================================================================

    g_taskbarCreated =
        RegisterWindowMessageW(L"TaskbarCreated");


    AddTrayIcon(
        g_hwnd,
        wc.hIconSm
    );


    // ========================================================================
    // Main message loop
    //
    // No polling.
    // No worker thread.
    // No Sleep().
    // No permanent timer.
    //
    // GetMessage() blocks until Windows actually has something to deliver.
    // ========================================================================

    MSG msg{};


    while (true)
    {
        const BOOL result =
            GetMessageW(
                &msg,
                nullptr,
                0,
                0
            );


        if (result == 0)
        {
            // WM_QUIT
            break;
        }


        if (result == -1)
        {
            // GetMessage failure.
            break;
        }


        TranslateMessage(
            &msg
        );


        DispatchMessageW(
            &msg
        );
    }


    // ========================================================================
    // Cleanup
    // ========================================================================

    if (g_hwnd != nullptr)
    {
        KillTimer(
            g_hwnd,
            TIMER_WAKE_CONFIRM
        );
    }


    if (g_powerNotify != nullptr)
    {
        UnregisterPowerSettingNotification(
            g_powerNotify
        );


        g_powerNotify = nullptr;
    }


    if (g_hwnd != nullptr)
    {
        UnregisterHotKey(
            g_hwnd,
            HOTKEY_BLACKOUT
        );


        if (IsWindow(g_hwnd))
        {
            DestroyWindow(
                g_hwnd
            );
        }


        g_hwnd = nullptr;
    }


    UnregisterClassW(
        WINDOW_CLASS,
        instance
    );


    if (g_mutex != nullptr)
    {
        CloseHandle(
            g_mutex
        );


        g_mutex = nullptr;
    }


    return 0;
}