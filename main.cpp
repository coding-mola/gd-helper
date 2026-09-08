/**
 * Grim Dawn 한글 입력 도우미 (Grim Dawn Hangeul Helper)
 * 
 * [최종 통합 픽스 내역]
 * 1. 입력창 활성화 시 전역 HotKey 즉시 해제 -> 입력창 내 한/영 키 자유 토글 지원 및 오작동 닫힘 방지
 * 2. Windows 10/11 새 IME 대응: ImmSetOpenStatus(TRUE) 및 강제 한글 모드 주입 보강
 * 3. AttachThreadInput 기반 안전한 포커스 전환 -> 게임 입력창 포커스 증발 및 붙여넣기 씹힘 원천 해결
 * 4. 포커스 복귀 대기 루프 강화: 최대 350ms 동안 게임 프로세스 활성화 재시도
 * 5. 다이렉트X 게임 인식용 하드웨어 스캔 코드(MapVirtualKeyW) 키 이벤트 적용
 * 6. 클립보드 경합 대응 재시도 루틴 및 메모리 안전 검증 유지
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
bool g_bHotkeyWarnedOnce = false;

// 게임 프로세스 감시 핸들
HANDLE g_hGameProcess = NULL;

// 클립보드 열기 재시도 루틴
bool OpenClipboardWithRetry(HWND hWnd, int maxAttempts = 5) {
    for (int i = 0; i < maxAttempts; ++i) {
        if (OpenClipboard(hWnd)) return true;
        Sleep(10);
    }
    return false;
}

// 클립보드 읽기
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

// 클립보드 쓰기
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

// DPI 인식 함수
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

// 그림던 프로세스 ID 검색
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

// 현재 활성 창이 그림던인지 판별
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

// 포커스 상태에 따른 단축키 동적 등록/해제
void UpdateHotkeyState(HWND hForeground) {
    if (!g_hWnd) return;

    // 도우미 입력창 자체가 활성화되어 있을 때는 단축키를 꺼서 한/영 키를 입력창이 온전히 쓰도록 보장
    if (hForeground == g_hWnd) {
        if (g_bHotkeyRegistered) {
            UnregisterHotKey(g_hWnd, HOTKEY_ID);
            g_bHotkeyRegistered = false;
        }
        return;
    }

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

// 전역 포커스 변경 감지 콜백
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd) {
        UpdateHotkeyState(hwnd);
    }
}

// 한글 입력 모드 강력 고정 (Windows 10/11 대응)
void ForceHangeulMode(HWND hEdit) {
    HKL hHangeulLayout = LoadKeyboardLayoutW(L"00000412", KLF_ACTIVATE);
    if (hHangeulLayout) {
        ActivateKeyboardLayout(hHangeulLayout, KLF_SETFORPROCESS);
    }

    HIMC hImc = ImmGetContext(hEdit);
    if (hImc) {
        ImmSetOpenStatus(hImc, TRUE); // [클로드 & Gemini 일치] IME를 반드시 열어야 한글 모드가 작동함

        DWORD dwConversion = 0, dwSentence = 0;
        ImmGetConversionStatus(hImc, &dwConversion, &dwSentence);
        if (!(dwConversion & IME_CMODE_HANGUL)) {
            dwConversion |= IME_CMODE_HANGUL;
            ImmSetConversionStatus(hImc, dwConversion, dwSentence);

            // Windows 11 새 IME API 무시 현상 대응: 가상 한/영 키 토글 주입
            DWORD checkConv = 0, checkSent = 0;
            ImmGetConversionStatus(hImc, &checkConv, &checkSent);
            if (!(checkConv & IME_CMODE_HANGUL)) {
                keybd_event(VK_HANGUL, 0, 0, 0);
                keybd_event(VK_HANGUL, 0, KEYEVENTF_KEYUP, 0);
            }
        }
        ImmReleaseContext(hEdit, hImc);
    }
}

// 게임 창으로 포커스 안전 복귀 및 키 주입
void PasteTextToGame(const std::wstring& text) {
    if (!g_hTargetGame || !IsWindow(g_hTargetGame)) return;

    // 1. 포커스 전환 준비 (스레드 입력 바인딩으로 포커스 탈취 보장)
    DWORD curThread = GetCurrentThreadId();
    DWORD targetThread = GetWindowThreadProcessId(g_hTargetGame, NULL);

    AttachThreadInput(curThread, targetThread, TRUE);
    SetForegroundWindow(g_hTargetGame);
    SetFocus(g_hTargetGame);
    AttachThreadInput(curThread, targetThread, FALSE);

    // 2. 도우미 창 숨기기
    ShowWindow(g_hWnd, SW_HIDE);

    // 빈 텍스트인 경우 키 전송 없이 포커스만 복귀
    if (text.empty()) return;

    // 3. 클립보드 백업 및 새 문자열 주입
    std::wstring prevClipboardText;
    bool hasBackup = GetClipboardUnicodeText(g_hWnd, prevClipboardText);

    if (!SetClipboardUnicodeText(g_hWnd, text)) return;

    // 4. 게임 창이 활성화될 때까지 재시도 루프 (클로드 제안 반영: 최대 350ms 대기)
    DWORD targetPid = 0;
    GetWindowThreadProcessId(g_hTargetGame, &targetPid);
    for (int i = 0; i < 15; ++i) { // 15회 * 20ms = 300ms
        DWORD fgPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
        if (fgPid == targetPid) break;
        Sleep(20);
    }
    Sleep(50);

    // 5. 다이렉트X 인식용 하드웨어 스캔 코드를 포함한 Ctrl + V 전송
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[0].ki.wScan = (WORD)MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC);

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[1].ki.wScan = (WORD)MapVirtualKeyW('V', MAPVK_VK_TO_VSC);

    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.wScan = (WORD)MapVirtualKeyW('V', MAPVK_VK_TO_VSC);
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;

    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.wScan = (WORD)MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC);
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(4, inputs, sizeof(INPUT));

    // 6. 이전 클립보드 내용 안전 복원
    if (hasBackup) {
        Sleep(250);
        SetClipboardUnicodeText(g_hWnd, prevClipboardText);
    }
}

// Edit 컨트롤 서브클래스 프로시저
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_KEYDOWN) {
        // Ctrl + A 전체 선택
        if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hWnd, EM_SETSEL, 0, -1);
            return 0;
        }
        else if (wParam == VK_RETURN) {
            // 한글 조합 강제 완료
            HIMC hImc = ImmGetContext(hWnd);
            if (hImc) {
                ImmNotifyIME(hImc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
                ImmReleaseContext(hWnd, hImc);
            }

            int len = GetWindowTextLengthW(hWnd);
            std::wstring buffer(len + 1, L'\0');
            GetWindowTextW(hWnd, &buffer[0], len + 1);
            buffer.resize(len);

            PasteTextToGame(buffer);
            return 0;
        }
        else if (wParam == VK_ESCAPE) {
            ShowWindow(g_hWnd, SW_HIDE);
            if (g_hTargetGame && IsWindow(g_hTargetGame)) {
                SetForegroundWindow(g_hTargetGame);
            }
            return 0;
        }
    }
    return CallWindowProc(g_OriginalEditProc, hWnd, uMsg, wParam, lParam);
}

// 메인 윈도우 프로시저
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
            // 현재 게임 창 핸들 기록
            g_hTargetGame = GetForegroundWindow();

            // 도우미 창이 열리는 동안 단축키를 해제하여 입력창 안에서 한/영 키를 자유롭게 쓰도록 함
            if (g_bHotkeyRegistered) {
                UnregisterHotKey(hWnd, HOTKEY_ID);
                g_bHotkeyRegistered = false;
            }

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

            // 화면 경계 보정
            if (posX + winW > mi.rcWork.right)  posX = mi.rcWork.right - winW - 8;
            if (posX < mi.rcWork.left)          posX = mi.rcWork.left + 8;
            if (posY + winH > mi.rcWork.bottom) posY = mi.rcWork.bottom - winH - 8;
            if (posY < mi.rcWork.top)           posY = pt.y + MulDiv(25, dpi, 96);

            SetWindowTextW(g_hEdit, L"");
            SetWindowPos(hWnd, HWND_TOPMOST, posX, posY, winW, winH, SWP_SHOWWINDOW);
            SetForegroundWindow(hWnd);
            SetFocus(g_hEdit);

            // 포커스 진입 즉시 한글 모드로 강제 고정
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
            AppendMenuW(hMenu, MF_STRING | MF_GRAYED, ID_TRAY_TITLE, L"그림던 한글 도우미 v1.4 (Final)");
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

// 프로그램 진입점
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
