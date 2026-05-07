#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <random>
#include <algorithm>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

using namespace Gdiplus;

// ===================== 资源与控件ID =====================
#define IDI_ICON1   101
#define IDB_PNG1    103
#define IDB_PNG2    102

#define BTN_PREV    1001
#define BTN_NEXT    1003
#define STATIC_TITLE 1004

// ===================== 全局变量 =====================
std::wstring g_musicDir = L"D:\\Music";
std::vector<std::wstring> g_musicList;
std::atomic<int> g_currentIndex(-1);
std::atomic<bool> g_shouldExit(false);
std::atomic<bool> g_songChanged(false);
std::thread g_playThread;
HWND g_hwndMain = NULL;
HWND g_hwndTitle = NULL;
ULONG_PTR g_gdiplusToken;

const WCHAR* MCI_ALIAS = L"MusicPlayer";

// ===================== 工具函数 =====================
std::wstring GetFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

void ScanMusicFiles() {
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW((g_musicDir + L"\\*.mp3").c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        MessageBoxW(g_hwndMain, L"未找到MP3文件", L"提示", MB_OK);
        return;
    }

    g_musicList.clear();
    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            g_musicList.push_back(g_musicDir + L"\\" + findData.cFileName);
        }
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);

    if (!g_musicList.empty()) {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(g_musicList.begin(), g_musicList.end(), g);
    }
}

void UpdateTitle() {
    if (!g_hwndTitle) return;
    std::wstring text = L"未播放歌曲";
    int idx = g_currentIndex.load();
    if (idx >= 0 && idx < (int)g_musicList.size()) {
        text = L"当前播放：" + GetFileName(g_musicList[idx]);
    }
    SetWindowTextW(g_hwndTitle, text.c_str());
}

// ===================== 核心：播放指定索引的歌曲 =====================
bool PlaySongAtIndex(int index) {
    if (g_musicList.empty() || index < 0 || index >= (int)g_musicList.size()) {
        return false;
    }

    wchar_t cmd[256];

    // 1. 停止并关闭当前播放（如果有的话）
    mciSendStringW(L"stop MusicPlayer", NULL, 0, NULL);
    mciSendStringW(L"close MusicPlayer", NULL, 0, NULL);

    // 2. 打开新歌曲
    wsprintfW(cmd, L"open \"%s\" alias %s", g_musicList[index].c_str(), MCI_ALIAS);
    MCIERROR err = mciSendStringW(cmd, NULL, 0, NULL);
    if (err != 0) {
        MessageBoxW(g_hwndMain, L"打开音频失败", L"错误", MB_ICONERROR);
        return false;
    }

    // 3. 播放
    wsprintfW(cmd, L"play %s", MCI_ALIAS);
    err = mciSendStringW(cmd, NULL, 0, NULL);
    if (err != 0) {
        MessageBoxW(g_hwndMain, L"播放音频失败", L"错误", MB_ICONERROR);
        return false;
    }

    return true;
}

// ===================== 切歌逻辑（主动触发） =====================
void PrevSong() {
    if (g_musicList.empty()) return;

    int newIndex = (g_currentIndex.load() - 1 + g_musicList.size()) % g_musicList.size();
    g_currentIndex.store(newIndex);
    g_songChanged.store(true);
    UpdateTitle();
}

void NextSong() {
    if (g_musicList.empty()) return;

    int newIndex = (g_currentIndex.load() + 1) % g_musicList.size();
    g_currentIndex.store(newIndex);
    g_songChanged.store(true);
    UpdateTitle();
}

// ===================== 播放线程（核心修复） =====================
void PlayThreadFunc() {
    while (!g_shouldExit.load()) {
        int currentIndex = g_currentIndex.load();

        if (currentIndex >= 0 && currentIndex < (int)g_musicList.size()) {
            // 尝试播放当前歌曲
            if (PlaySongAtIndex(currentIndex)) {
                // 等待播放结束或手动切歌
                bool isPlayFinished = false;
                while (!g_shouldExit.load()) {
                    wchar_t status[32] = { 0 };
                    mciSendStringW(L"status MusicPlayer mode", status, 32, NULL);

                    // 手动切歌：重置标志并跳出
                    if (g_songChanged.load()) {
                        g_songChanged.store(false);
                        break;
                    }

                    // 歌曲自然播放结束：标记并跳出
                    if (wcscmp(status, L"stopped") == 0) {
                        isPlayFinished = true;
                        break;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                // 核心修复：歌曲自然播放结束后，自动切换下一首
                if (isPlayFinished && !g_shouldExit.load()) {
                    int newIndex = (currentIndex + 1) % g_musicList.size();
                    g_currentIndex.store(newIndex);
                    UpdateTitle(); // 同步更新标题
                }
            }
            else {
                // 播放出错，自动下一首
                int newIndex = (currentIndex + 1) % g_musicList.size();
                g_currentIndex.store(newIndex);
                g_songChanged.store(true);
            }
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // 退出清理
    mciSendStringW(L"stop MusicPlayer", NULL, 0, NULL);
    mciSendStringW(L"close MusicPlayer", NULL, 0, NULL);
}

// ===================== GDI+ 绘制 =====================
void DrawPNG(HDC hdc, int resID, int x, int y, int width = 0, int height = 0) {
    HMODULE hModule = GetModuleHandleW(NULL);
    HRSRC hRes = FindResourceW(hModule, MAKEINTRESOURCE(resID), L"PNG");
    if (!hRes) return;

    HGLOBAL hMem = LoadResource(hModule, hRes);
    if (!hMem) return;

    DWORD size = SizeofResource(hModule, hRes);
    LPVOID pData = LockResource(hMem);

    IStream* pStream = NULL;
    (void)CreateStreamOnHGlobal(NULL, TRUE, &pStream);
    pStream->Write(pData, size, NULL);
    LARGE_INTEGER li = { 0 };
    pStream->Seek(li, STREAM_SEEK_SET, NULL);

    Image image(pStream);
    pStream->Release();

    Graphics graphics(hdc);
    int imgWidth = image.GetWidth();
    int imgHeight = image.GetHeight();

    if (width == 0 || height == 0) {
        graphics.DrawImage(&image, x, y);
    }
    else {
        graphics.DrawImage(&image, x, y, width, height);
    }
}

// ===================== 窗口过程 =====================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hwndMain = hwnd;
        HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;

        // 创建按钮和标题框
        CreateWindowW(L"BUTTON", L"上一首",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            80, 80, 80, 35,
            hwnd, (HMENU)BTN_PREV, hInst, NULL);

        CreateWindowW(L"BUTTON", L"下一首",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            240, 80, 80, 35,
            hwnd, (HMENU)BTN_NEXT, hInst, NULL);

        g_hwndTitle = CreateWindowW(L"STATIC", L"未播放歌曲",
            WS_CHILD | WS_VISIBLE | SS_CENTER | WS_BORDER,
            80, 130, 240, 30,
            hwnd, (HMENU)STATIC_TITLE, hInst, NULL);

        // 初始化播放器
        ScanMusicFiles();
        if (!g_musicList.empty()) {
            g_currentIndex.store(0);
            UpdateTitle();
            if (!g_playThread.joinable()) {
                g_playThread = std::thread(PlayThreadFunc);
            }
        }
        else {
            MessageBoxW(hwnd, L"音乐目录为空", L"警告", MB_OK);
        }
        break;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case BTN_PREV: PrevSong(); break;
        case BTN_NEXT: NextSong(); break;
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);

        // 1. 绘制背景
        DrawPNG(hdc, IDB_PNG2, 0, 0, rcClient.right, rcClient.bottom);

        // 2. 调用 DefWindowProc 绘制控件（确保按钮在最上层）
        DefWindowProcW(hwnd, msg, wParam, lParam);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY: {
        g_shouldExit.store(true);
        mciSendStringW(L"stop MusicPlayer", NULL, 0, NULL);

        if (g_playThread.joinable()) {
            g_playThread.join();
        }
        PostQuitMessage(0);
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ===================== 程序入口 =====================
int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow) {

    // GDI+ 初始化
    GdiplusStartupInput gdiplusInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusInput, NULL);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MusicPlayerClass";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_ICON1));

    // 添加 CS_HREDRAW 和 CS_VREDRAW 样式，减少闪烁
    wc.style = CS_HREDRAW | CS_VREDRAW;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"窗口类注册失败", L"错误", MB_ICONERROR);
        GdiplusShutdown(g_gdiplusToken);
        return 1;
    }

    HWND hwnd = CreateWindowW(
        L"MusicPlayerClass", L"多线程音乐播放器",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 220,
        NULL, NULL, hInstance, NULL);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = { 0 };
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}
