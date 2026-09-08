/**
 * Grim Dawn 한글 입력 도우미 (Grim Dawn Hangeul Helper)
 * 
 * [최적화 및 안정화 내역]
 * 1. 클립보드 메모리 잠금(GlobalLock) 2중 널 포인터 검증 (크래시 방어)
 * 2. 프레임 드랍 대응: 클립보드 복원 대기 시간 250ms 확보
 * 3. 단축키 실패 알림 중복 스팸 방지 (최초 1회만 노출 & 전역 uFlags 오염 방지)
 * 4. 포커스 전환에 따른 동적 HotKey 등록/해제
 * 5. Edit 컨트롤 서브클래스 프로시저 안전 복원 (WM_DESTROY)
 * 6. 게임 창 생존 검사(IsWindow) 및 포커스 복귀 실패 방어
 * 7. 클립보드 열기/주입 실패 시 가상 키 전송 차단
 * 8. 빈 입력 Enter 시 게임 포커스 복귀 보장
 * 9. Edit 컨트롤 내 Ctrl + A (전체 선택) 지원
 * 10. 클립보드 경합 대응 재시도 루틴 (OpenClipboardWithRetry)
 * 11. ForceHangeulMode 함수 및 화면 상단 경계 보정(rcWork.top) 복원
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <imm.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// 식별자 정의
#define HOTKEY_ID          1
#define IDC_EDIT_INPUT     101
#define WM_TRAYICON        (WM_APP + 1)
#define ID_TRAY_EXIT       2001
#define ID_TRAY_TITLE      2002
#define TIMER_CHECK_GAME   3001

// 전역 핸들 및 상태
HWND g_hWnd = NULL;
HWND g_hEdit = NULL;
HWND g_hTargetGame = NULL;
HFONT g_hFont = NULL;
UINT g_nLastDpi = 0;
WNDPROC g_OriginalEditProc = NULL;
HWINEVENTHOOK g_hEventHook = NULL;
NOTIFYICONDATAW g_nid = {};
bool g_bHotkeyRegistered = false;
bool g_bHotkeyWarnedOnce = false; // 알림 중복 스팸 방지 플래그

// 게임 프로세스 감시 핸들
HANDLE g_hGameProcess = NULL;

// 클립보드 찰나의 잠김(타 프로그램 경합) 방어용 재시도 OpenClipboard
bool OpenClipboardWithRetry(HWND hWnd, int maxAttempts = 5) {
    for (int i = 0; i < maxAttempts; ++i) {
        if (OpenClipboard(hWnd)) return true;
        Sleep(10);
    }
    return false;
}

// 클립보드 텍스트 읽기 헬퍼
bool GetClipboardUnicodeText(HWND hWnd, std::wstring& outText) {
    if (!OpenClipboardWithRetry(hWnd)) return false;
    bool success = false;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        wchar_t* pText = (wchar_t*)GlobalLock(hData);
        if (pText) {
            outText = pText;
            success = true;
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return success;
}

// 클립보드 텍스트 쓰기 헬퍼 (2중 널 검증 내장)
bool SetClipboardUnicodeText(HWND hWnd, const std::wstring& text) {
    if (!OpenClipboardWithRetry(hWnd)) return false;
    EmptyClipboard();
    bool success = false;
    size_t bytes = (text.length() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* pDest = GlobalLock(hMem);
        if (pDest) {
            memcpy(pDest, text.c_str(), bytes);
            GlobalUnlock(hMem);
            if (SetClipboardData(CF_UNICODETEXT, hMem)) {
                success = true;
            }
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
    return success;
}

// 1. DPI 인식 함수 (동적 로딩)
void EnableDpiAwareness() {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL(WINAPI* SetDpiAwareContextProc)(DPI_AWARENESS_CONTEXT);
        SetDpiAwareContextProc pSetDpi = (SetDpiAwareContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (pSetDpi) {
            pSetDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
        }
    }
}

UINT GetWindowDpi(HWND hWnd) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef UINT(WINAPI* GetDpiProc)(HWND);
        GetDpiProc pGetDpi = (GetDpiProc)GetProcAddress(hUser32, "GetDpiForWindow");
        if (pGetDpi) return pGetDpi(hWnd);
    }
    HDC hdc = GetDC(hWnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hWnd, hdc);
    return (dpi > 0) ? dpi : 96;
}

// 2. 그림던 프로세스 ID 검색
DWORD FindGrimDawnProcessId() {
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    DWORD targetPid = 0;
    if (Process32FirstW(hSnap, &pe)) {
        do {
            std::wstring name = pe.szExeFile;
            for (auto& c : name) c = towlower(c);
            if (name == L"grim dawn.exe" || name == L"grim dawn (x64).exe" || name == L"grimdawn.exe") {
                targetPid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return targetPid;
}

// 3. 현재 활성 창이 그림던인지 판별
bool IsTargetGameWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return false;

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return false;

    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL success = QueryFullProcessImageNameW(hProc, 0, exePath, &size);
    CloseHandle(hProc);

    if (!success) return false;

    for (DWORD i = 0; i < size; ++i) {
        exePath[i] = towlower(exePath[i]);
    }

    return (wcsstr(exePath, L"grim dawn.exe") != nullptr ||
            wcsstr(exePath, L"grim dawn (x64).exe") != nullptr ||
            wcsstr(exePath, L"grimdawn.exe") != nullptr);
}

// 4. 포커스 상태에 따른 단축키 동적 등록/해제
void UpdateHotkeyState(HWND hForeground) {
    if (!g_hWnd || hForeground == g_hWnd) return;

    bool isGame = IsTargetGameWindow(hForeground);
    if (isGame) {
        if (!g_bHotkeyRegistered) {
            if (RegisterHotKey(g_hWnd, HOTKEY_ID, 0, VK_HANGUL)) {
                g_bHotkeyRegistered = true;
                g_bHotkeyWarnedOnce = false;
            } else {
                if (!g_bHotkeyWarnedOnce) {
                    NOTIFYICONDATAW nidWarn = g_nid;
                    nidWarn.uFlags |= NIF_INFO;
                    wcscpy_s(nidWarn.szInfoTitle, L"단축키 등록 실패");
                    wcscpy_s(nidWarn.szInfo, L"한/영 키를 다른 프로그램이 사용 중입니다.");
                    nidWarn.dwInfoFlags = NIIF_WARNING;
                    Shell_NotifyIconW(NIM_MODIFY, &nidWarn);
                    g_bHotkeyWarnedOnce = true;
                }
            }
        }
    } else {
        if (g_bHotkeyRegistered) {
            UnregisterHotKey(g_hWnd, HOTKEY_ID);
            g_bHotkeyRegistered = false;
        }
    }
}

// 5. 전역 포커스 변경 감지 콜백
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd) {
        UpdateHotkeyState(hwnd);
    }
}

// 6. 한글(Hangeul) 입력 모드 강제 주입 (복원 완료)
void ForceHangeulMode(HWND hEdit) {
    HKL hHangeulLayout = LoadKeyboardLayoutW(L"00000412", KLF_ACTIVATE);
    if (hHangeulLayout) {
        ActivateKeyboardLayout(hHangeulLayout, KLF_SETFORPROCESS);
    }

    HIMC hImc = ImmGetContext(hEdit);
    if (hImc) {
        DWORD dwConversion = 0, dwSentence = 0;
        if (ImmGetConversionStatus(hImc, &dwConversion, &dwSentence)) {
            dwConversion |= IME_CMODE_HANGUL;
            ImmSetConversionStatus(hImc, dwConversion, dwSentence);
        }
        ImmReleaseContext(hEdit, hImc);
    }
}

// 7. 클립보드 백업 -> 한글 주입 -> 클립보드 안전 복원
void PasteTextToGame(const std::wstring& text) {
    if (!g_hTargetGame || !IsWindow(g_hTargetGame)) return;

    // 빈 텍스트 입력 시 키 전송 없이 게임 포커스만 복귀
    if (text.empty()) {
        SetForegroundWindow(g_hTargetGame);
        return;
    }

    std::wstring prevClipboardText;
    bool hasBackup = GetClipboardUnicodeText(g_hWnd, prevClipboardText);

    if (!SetClipboardUnicodeText(g_hWnd, text)) return;

    SetForegroundWindow(g_hTargetGame);
    Sleep(50);

    if (GetForegroundWindow() != g_hTargetGame) {
        if (hasBackup) SetClipboardUnicodeText(g_hWnd, prevClipboardText);
        return;
    }

    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(4, inputs, sizeof(INPUT));

    if (hasBackup) {
        Sleep(250);
        SetClipboardUnicodeText(g_hWnd, prevClipboardText);
    }
}

// 8. Edit 컨트롤 서브클래스 프로시저
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN) {
        // Ctrl + A 전체 선택
        if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
            return 0;
        }
        else if (wParam == VK_RETURN) {
            HIMC hImc = ImmGetContext(hWnd);
            if (hImc) {
                ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
                ImmReleaseContext(hWnd, hImc);
            }

            int len = GetWindowTextLengthW(hWnd);
            std::wstring buffer(len + 1, L'\0');
            GetWindowTextW(hWnd, &buffer[0], len + 1);
            buffer.resize(len);

            ShowWindow(g_hWnd, SW_HIDE);
            PasteTextToGame(buffer);
            return 0;
        }
        else if (wParam == VK_ESCAPE) {
            ShowWindow(g_hWnd, SW_HIDE);
            if (g_hTargetGame && IsWindow(g_hTargetGame)) SetForegroundWindow(g_hTargetGame);
            return 0;
        }
    }
    return CallWindowProc(g_OriginalEditProc, hWnd, uMsg, wParam, lParam);
}

// 9. 메인 오버레이 윈도우 프로시저
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_hEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            4, 4, 252, 28, hWnd, (HMENU)IDC_EDIT_INPUT, GetModuleHandle(NULL), NULL);

        g_OriginalEditProc = (WNDPROC)SetWindowLongPtr(g_hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        UINT dpi = GetWindowDpi(hWnd);
        g_nLastDpi = dpi;
        int fontHeight = MulDiv(18, dpi, 96);
        g_hFont = CreateFontW(fontHeight, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
        SendMessage(g_hEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_nid.cbSize = sizeof(NOTIFYICONDATAW);
        g_nid.hWnd = hWnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wcscpy_s(g_nid.szTip, L"그림던 한글 입력 도우미 (실행 중)");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        SetTimer(hWnd, TIMER_CHECK_GAME, 1000, NULL);
        break;
    }
    case WM_TIMER: {
        if (wParam == TIMER_CHECK_GAME) {
            if (!g_hGameProcess) {
                DWORD pid = FindGrimDawnProcessId();
                if (pid != 0) {
                    g_hGameProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
                }
            } else {
                if (WaitForSingleObject(g_hGameProcess, 0) == WAIT_OBJECT_0) {
                    CloseHandle(g_hGameProcess);
                    g_hGameProcess = NULL;
                    KillTimer(hWnd, TIMER_CHECK_GAME);
                    DestroyWindow(hWnd);
                }
            }
        }
        break;
    }
    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        if (g_hEdit) {
            MoveWindow(g_hEdit, 4, 4, w - 8, h - 8, TRUE);
        }
        break;
    }
    case WM_HOTKEY: {
        if (wParam == HOTKEY_ID) {
            if (IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, SW_HIDE);
                if (g_hTargetGame && IsWindow(g_hTargetGame)) SetForegroundWindow(g_hTargetGame);
                break;
            }

            g_hTargetGame = GetForegroundWindow();

            POINT pt;
            GetCursorPos(&pt);

            HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(mi) };
            GetMonitorInfoW(hMon, &mi);

            UINT dpi = GetWindowDpi(hWnd);
            if (dpi != g_nLastDpi) {
                g_nLastDpi = dpi;
                if (g_hFont) DeleteObject(g_hFont);
                int fontHeight = MulDiv(18, dpi, 96);
                g_hFont = CreateFontW(fontHeight, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
                SendMessage(g_hEdit, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            }

            int winW = MulDiv(260, dpi, 96);
            int winH = MulDiv(36, dpi, 96);

            int posX = pt.x - (winW / 2);
            int posY = pt.y - winH - MulDiv(10, dpi, 96);

            // 화면 경계 보정 (mi.rcWork.top 복원)
            if (posX + winW > mi.rcWork.right)  posX = mi.rcWork.right - winW - 8;
            if (posX < mi.rcWork.left)          posX = mi.rcWork.left + 8;
            if (posY + winH > mi.rcWork.bottom) posY = mi.rcWork.bottom - winH - 8;
            if (posY < mi.rcWork.top)           posY = pt.y + MulDiv(25, dpi, 96);

            SetWindowTextW(g_hEdit, L"");
            SetWindowPos(hWnd, HWND_TOPMOST, posX, posY, winW, winH, SWP_SHOWWINDOW);
            SetForegroundWindow(hWnd);
            SetFocus(g_hEdit);

            ForceHangeulMode(g_hEdit);
        }
        break;
    }
    case WM_ACTIVATE: {
        if (LOWORD(wParam) == WA_INACTIVE) {
            ShowWindow(hWnd, SW_HIDE);
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(245, 245, 245));
        SetBkColor(hdc, RGB(36, 36, 36));
        static HBRUSH hbrEdit = CreateSolidBrush(RGB(36, 36, 36));
        return (LRESULT)hbrEdit;
    }
    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP) {
            POINT curPoint;
            GetCursorPos(&curPoint);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING | MF_GRAYED, ID_TRAY_TITLE, L"그림던 한글 도우미 v1.2");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"종료 (&Exit)");

            SetForegroundWindow(hWnd);
            int selected = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, curPoint.x, curPoint.y, 0, hWnd, NULL);
            DestroyMenu(hMenu);

            if (selected == ID_TRAY_EXIT) {
                DestroyWindow(hWnd);
            }
        }
        break;
    }
    case WM_DESTROY: {
        KillTimer(hWnd, TIMER_CHECK_GAME);
        if (g_hGameProcess) {
            CloseHandle(g_hGameProcess);
            g_hGameProcess = NULL;
        }
        if (g_hEventHook) UnhookWinEvent(g_hEventHook);
        if (g_bHotkeyRegistered) UnregisterHotKey(hWnd, HOTKEY_ID);
        
        if (g_hEdit && g_OriginalEditProc) {
            SetWindowLongPtr(g_hEdit, GWLP_WNDPROC, (LONG_PTR)g_OriginalEditProc);
        }

        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// 10. 프로그램 진입점
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"GrimDawn_Hangeul_Helper_Final_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"그림던 한글 입력 도우미가 이미 실행 중입니다.", L"알림", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    EnableDpiAwareness();

    const wchar_t CLASS_NAME[] = L"GD_Hangeul_Overlay_Final_Class";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 24));
    RegisterClassW(&wc);

    g_hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        CLASS_NAME, L"GD_Hangeul_Overlay", WS_POPUP,
        0, 0, 260, 36, NULL, NULL, hInstance, NULL);

    if (!g_hWnd) return 0;

    SetLayeredWindowAttributes(g_hWnd, 0, 210, LWA_ALPHA);

    g_hEventHook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        NULL, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    UpdateHotkeyState(GetForegroundWindow());

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return 0;
}
