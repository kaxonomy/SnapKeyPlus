// SnapKey 1.2.6
// github.com/cafali/SnapKey

#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <regex>
#include <random>
#include <thread>
#include <chrono>

using namespace std;

#define ID_TRAY_APP_ICON                1001
#define ID_TRAY_EXIT_CONTEXT_MENU_ITEM  3000
#define ID_TRAY_VERSION_INFO            3001
#define ID_TRAY_REBIND_KEYS             3002
#define ID_TRAY_LOCK_FUNCTION           3003
#define ID_TRAY_RESTART_SNAPKEY         3004
#define WM_TRAYICON                     (WM_USER + 1)

struct KeyState
{
    bool registered = false;
    bool keyDown = false;
    int group;
    bool simulated = false;
};

struct GroupState
{
    int previousKey;
    int activeKey;
};

unordered_map<int, GroupState> GroupInfo;
unordered_map<int, KeyState> KeyInfo;

HHOOK hHook = NULL;
HANDLE hMutex = NULL;
NOTIFYICONDATA nid;
bool isLocked = false; // Variable to track the lock state

// Function declarations
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void InitNotifyIconData(HWND hwnd);
bool LoadConfig(const std::wstring& filename);
void CreateDefaultConfig(const std::wstring& filename); // Declaration
void RestoreConfigFromBackup(const std::wstring& backupFilename, const std::wstring& destinationFilename); // Declaration
std::wstring GetVersionInfo(); // Declaration
void SendKey(int target, bool keyDown);

// Random delay settings
int minDelay = 1;
int maxDelay = 2;

void addRandomDelay()
{
    // Create a random number generator
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(minDelay, maxDelay);

    // Generate a random delay duration
    int delayDuration = dis(gen);

    // Sleep for the random duration
    std::this_thread::sleep_for(std::chrono::milliseconds(delayDuration));
}

int main()
{
    // Load key bindings (config file)
    if (!LoadConfig(L"config.cfg")) {
        return 1;
    }

    // One instance restriction
    hMutex = CreateMutex(NULL, TRUE, TEXT("SnapKeyMutex"));
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBox(NULL, TEXT("SnapKey is already running!"), TEXT("SnapKey"), MB_ICONINFORMATION | MB_OK);
        return 1; // Exit the program
    }

    // Create a window class
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = TEXT("SnapKeyClass");

    if (!RegisterClassEx(&wc)) {
        MessageBox(NULL, TEXT("Window Registration Failed!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // Create a window
    HWND hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        TEXT("SnapKey"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 240, 120,
        NULL, NULL, wc.hInstance, NULL);

    if (hwnd == NULL) {
        MessageBox(NULL, TEXT("Window Creation Failed!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // Initialize and add the system tray icon
    InitNotifyIconData(hwnd);

    // Set the hook
    hHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);
    if (hHook == NULL)
    {
        MessageBox(NULL, TEXT("Failed to install hook!"), TEXT("Error"), MB_ICONEXCLAMATION | MB_OK);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 1;
    }

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Unhook the hook
    UnhookWindowsHookEx(hHook);

    // Remove the system tray icon
    Shell_NotifyIcon(NIM_DELETE, &nid);

    // Release and close the mutex
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);

    return 0;
}

void handleKeyDown(int keyCode)
{
    KeyState& currentKeyInfo = KeyInfo[keyCode];
    GroupState& currentGroupInfo = GroupInfo[currentKeyInfo.group];
    if (!currentKeyInfo.keyDown)
    {
        addRandomDelay();
        currentKeyInfo.keyDown = true;
        SendKey(keyCode, true);
        if (currentGroupInfo.activeKey == 0 || currentGroupInfo.activeKey == keyCode)
        {
            currentGroupInfo.activeKey = keyCode;
        }
        else
        {
            currentGroupInfo.previousKey = currentGroupInfo.activeKey;
            currentGroupInfo.activeKey = keyCode;

            SendKey(currentGroupInfo.previousKey, false);
        }
    }
}

void handleKeyUp(int keyCode)
{
    KeyState& currentKeyInfo = KeyInfo[keyCode];
    GroupState& currentGroupInfo = GroupInfo[currentKeyInfo.group];
    if (currentKeyInfo.keyDown)
    {
        addRandomDelay();

        currentKeyInfo.keyDown = false;

        if (currentGroupInfo.activeKey == keyCode)
        {
            SendKey(keyCode, false);
            currentGroupInfo.activeKey = currentGroupInfo.previousKey;
            currentGroupInfo.previousKey = 0;
        }
        else if (currentGroupInfo.previousKey == keyCode)
        {
            currentGroupInfo.previousKey = 0;
        }
    }
}

bool isSimulatedKeyEvent(DWORD flags) {
    return flags & 0x10;
}

void SendKey(int targetKey, bool keyDown)
{
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = targetKey;
    input.ki.wScan = MapVirtualKey(targetKey, 0);
    input.ki.dwFlags = keyDown ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (!isLocked && nCode >= 0)
    {
        KBDLLHOOKSTRUCT *pKeyBoard = (KBDLLHOOKSTRUCT *)lParam;
        if (!isSimulatedKeyEvent(pKeyBoard->flags))
        {
            if (KeyInfo[pKeyBoard->vkCode].registered)
            {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
                {
                    handleKeyDown(pKeyBoard->vkCode);
                }
                if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
                {
                    handleKeyUp(pKeyBoard->vkCode);
                }
                return 1;
            }
        }
    }
    return CallNextHookEx(hHook, nCode, wParam, lParam);
}

void InitNotifyIconData(HWND hwnd)
{
    memset(&nid, 0, sizeof(NOTIFYICONDATA));

    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY_APP_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;

    // Load the tray icon (current directory)
    HICON hIcon = (HICON)LoadImage(NULL, TEXT("icon.ico"), IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    if (hIcon)
    {
        nid.hIcon = hIcon;
    }
    else
    {
        // If loading the icon fails, fallback to a default icon
        nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }

    lstrcpy(nid.szTip, TEXT("SnapKey"));

    Shell_NotifyIcon(NIM_ADD, &nid);
}

std::wstring GetVersionInfo() // Get version info
{
    std::wifstream versionFile(L"meta/version");
    if (!versionFile.is_open()) {
        return L"Version info not available";
    }

    std::wstring version;
    std::getline(versionFile, version);
    return version.empty() ? L"Version info not available" : version;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONDOWN)
        {
            POINT curPoint;
            GetCursorPos(&curPoint);
            SetForegroundWindow(hwnd);

            // Create context menu
            HMENU hMenu = CreatePopupMenu();
            AppendMenu(hMenu, MF_STRING, ID_TRAY_REBIND_KEYS, TEXT("Rebind Keys"));
            AppendMenu(hMenu, MF_STRING | (isLocked ? MF_CHECKED : MF_UNCHECKED), ID_TRAY_LOCK_FUNCTION, TEXT("Disable SnapKey"));
            AppendMenu(hMenu, MF_STRING, ID_TRAY_RESTART_SNAPKEY, TEXT("Restart SnapKey"));
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_TRAY_VERSION_INFO, TEXT("Version Info"));
            AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT_CONTEXT_MENU_ITEM, TEXT("Exit SnapKey"));

            // Display the context menu
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, curPoint.x, curPoint.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        else if (lParam == WM_LBUTTONDBLCLK) //double-click tray icon
        {
            // Toggle lock state
            isLocked = !isLocked;

            // Update the tray icon
            if (isLocked)
            {
                // Load icon_off.ico (OFF)
                HICON hIconOff = (HICON)LoadImage(NULL, TEXT("icon_off.ico"), IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
                if (hIconOff)
                {
                    nid.hIcon = hIconOff;
                    Shell_NotifyIcon(NIM_MODIFY, &nid);
                    DestroyIcon(hIconOff);
                }
            }
            else
            {
                // Load icon.ico (ON)
                HICON hIconOn = (HICON)LoadImage(NULL, TEXT("icon.ico"), IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
                if (hIconOn)
                {
                    nid.hIcon = hIconOn;
                    Shell_NotifyIcon(NIM_MODIFY, &nid);
                    DestroyIcon(hIconOn);
                }
            }
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_TRAY_EXIT_CONTEXT_MENU_ITEM:
            PostQuitMessage(0);
            break;
        case ID_TRAY_VERSION_INFO:
            {
                std::wstring versionInfo = GetVersionInfo();
                MessageBox(hwnd, versionInfo.c_str(), TEXT("Version Info"), MB_OK);
                
            }
            break;
        case ID_TRAY_REBIND_KEYS:
            {
                TCHAR szExeFileName[MAX_PATH];
                GetModuleFileName(NULL, szExeFileName, MAX_PATH);
                std::wstring exePath = szExeFileName;
                std::wstring exeDir = exePath.substr(0, exePath.find_last_of(L"\\/"));
                std::wstring configPath = exeDir + L"\\config.cfg";
                ShellExecute(NULL, TEXT("open"), configPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }
            break;
        case ID_TRAY_RESTART_SNAPKEY:
            {
                TCHAR szExeFileName[MAX_PATH];
                GetModuleFileName(NULL, szExeFileName, MAX_PATH);
                ShellExecute(NULL, NULL, szExeFileName, NULL, NULL, SW_SHOWNORMAL);
                PostQuitMessage(0);
            }
            break;
        case ID_TRAY_LOCK_FUNCTION:
            {
                isLocked = !isLocked;
                HICON hIcon = isLocked
                    ? (HICON)LoadImage(NULL, TEXT("icon_off.ico"), IMAGE_ICON, 0, 0, LR_LOADFROMFILE)
                    : (HICON)LoadImage(NULL, TEXT("icon.ico"), IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
                if (hIcon)
                {
                    nid.hIcon = hIcon;
                    Shell_NotifyIcon(NIM_MODIFY, &nid);
                    DestroyIcon(hIcon);
                }
            }
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// Function to copy snapkey.backup (meta folder) to the main directory
void RestoreConfigFromBackup(const std::wstring& backupFilename, const std::wstring& destinationFilename)
{
    // Get the executable path
    TCHAR exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);

    // Convert wide-character path to narrow string
    std::wstring exeDir = exePath;
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));

    std::wstring sourcePath = exeDir + L"\\meta\\" + backupFilename;
    std::wstring destinationPath = exeDir + L"\\" + destinationFilename;

    if (CopyFile(sourcePath.c_str(), destinationPath.c_str(), FALSE)) {
        // Copy successful
        MessageBox(NULL, TEXT("Default config restored from backup successfully."), TEXT("SnapKey"), MB_ICONINFORMATION | MB_OK);
    } else {
        // backup.snapkey copy failed
        DWORD error = GetLastError();
        std::wstring errorMsg = L"Failed to restore config from backup. Error code: " + std::to_wstring(error);
        MessageBox(NULL, errorMsg.c_str(), TEXT("SnapKey Error"), MB_ICONERROR | MB_OK);
    }
}

// Restore config.cfg from backup.snapkey
void CreateDefaultConfig(const std::wstring& filename)
{
    std::wstring backupFilename = L"backup.snapkey";
    RestoreConfigFromBackup(backupFilename, filename);
}

// Check for config.cfg
bool LoadConfig(const std::wstring& filename)
{
    // Get the executable path
    TCHAR exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);

    // Convert wide-character path to narrow string
    std::wstring exeDir = exePath;
    exeDir = exeDir.substr(0, exeDir.find_last_of(L"\\/"));

    std::wstring fullPath = exeDir + L"\\" + filename;

    std::wifstream configFile(fullPath);
    if (!configFile.is_open()) {
        CreateDefaultConfig(filename);  // Restore config from backup.snapkey if file doesn't exist
        return false;
    }

    std::wstring line; // Check for duplicated keys in the config file
    int id = 0;
    while (std::getline(configFile, line)) {
        std::wistringstream iss(line);
        std::wstring key;
        int value;
        std::wregex secPat(LR"(\\s*\\[Group\\]\\s*)");
        if (std::regex_match(line, secPat))
        {
            id++;
        }
        else if (std::getline(iss, key, L'=') && (iss >> value))
        {
            if (key.find(L"key") != std::wstring::npos)
            {
                if (!KeyInfo[value].registered)
                {
                    KeyInfo[value].registered = true;
                    KeyInfo[value].group = id;
                }
                else
                {
                    MessageBox(NULL, TEXT("The config file contains duplicate keys across various groups. Please review and correct the setup."), TEXT("SnapKey Error"), MB_ICONEXCLAMATION | MB_OK);
                    MessageBox(NULL, TEXT("For more information, please view the README file or visit github.com/cafali/SnapKey/wiki."), TEXT("SnapKey Error"), MB_ICONINFORMATION | MB_OK);
                    return false;
                }
            }
        }
        if (line.find(L"random_delay_ms=") == 0)
        {
            std::wsmatch match;
            std::wregex delayPattern(LR"(\\s*(\\d+)\\s*,\\s*(\\d+)\\s*)");
            if (std::regex_search(line, match, delayPattern) && match.size()) {
                minDelay = std::stoi(match[1].str());
                maxDelay = std::stoi(match[2].str());
            }
        }
    }
    return true;
}
