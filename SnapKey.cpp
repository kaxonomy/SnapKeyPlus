// SnapKey 1.2.8
// github.com/cafali/SnapKey

#include <windows.h>
#include <shellapi.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <regex>
#include <random>
#include <chrono>
#include <thread>

using namespace std;

#define ID_TRAY_APP_ICON                1001
#define ID_TRAY_EXIT_CONTEXT_MENU_ITEM  3000
#define ID_TRAY_VERSION_INFO            3001
#define ID_TRAY_REBIND_KEYS             3002
#define ID_TRAY_LOCK_FUNCTION           3003
#define ID_TRAY_RESTART_SNAPKEY         3004
#define ID_TRAY_HELP                    3005 // v1.2.8
#define ID_TRAY_CHECKUPDATE             3006 // v1.2.8
#define ID_TRAY_VAC_BYPASS_A            3007
#define ID_TRAY_VAC_BYPASS_B            3008
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
bool vacBypassAEnabled = false; // VAC bypass A toggle
bool vacBypassBEnabled = false; // VAC bypass B toggle
int vacCounter = 0;    // counter for imperfect snaptap
int vacAMinDelay = 15; // A mode minimum overlap delay
int vacAMaxDelay = 35; // A mode maximum overlap delay
int vacBMinDelay = 5;  // B mode minimum release-press interval
int vacBMaxDelay = 15; // B mode maximum release-press interval

static std::mt19937 rng(std::random_device{}());

// Function declarations
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void InitNotifyIconData(HWND hwnd);
bool LoadConfig(const std::string& filename);
void CreateDefaultConfig(const std::string& filename); 
void RestoreConfigFromBackup(const std::string& backupFilename, const std::string& destinationFilename);
std::string GetVersionInfo();
void SendKey(int target, bool keyDown);
void UpdateTrayIcon();
void WriteConfigValue(const std::string& filename, const std::string& key, int value);

// Random delay settings
int minDelay = 1;
int maxDelay = 2;

void addRandomDelay()
{
    // Create a random number generator
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(minDelay, maxDelay);
    int delayDuration = dis(gen);

    // Sleep for the random duration
    std::this_thread::sleep_for(std::chrono::milliseconds(delayDuration));
}

int main()
{
    // Load key bindings (config file)
    if (!LoadConfig("config.cfg")) {
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
    UpdateTrayIcon();

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
        currentKeyInfo.keyDown = true;
        if (currentGroupInfo.activeKey == 0 || currentGroupInfo.activeKey == keyCode)
        {
            SendKey(keyCode, true);
            currentGroupInfo.activeKey = keyCode;
        }
        else
        {
            currentGroupInfo.previousKey = currentGroupInfo.activeKey;
            currentGroupInfo.activeKey = keyCode;

            if (vacBypassBEnabled && std::uniform_int_distribution<int>(0,1)(rng) == 0)
            {
                SendKey(currentGroupInfo.previousKey, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::uniform_int_distribution<int>(vacBMinDelay, vacBMaxDelay)(rng)));
                SendKey(keyCode, true);
            }
            else
            {
                SendKey(keyCode, true);
                if (vacBypassAEnabled)
                {
                    if (vacCounter >= 17)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(
                            std::uniform_int_distribution<int>(vacAMinDelay, vacAMaxDelay)(rng)));
                        vacCounter = 0;
                    }
                    else
                    {
                        vacCounter++;
                    }
                }
                SendKey(currentGroupInfo.previousKey, false);
            }
        }
    }
}

void handleKeyUp(int keyCode)

{
    KeyState& currentKeyInfo = KeyInfo[keyCode];

    GroupState& currentGroupInfo = GroupInfo[currentKeyInfo.group];

    if (currentGroupInfo.previousKey == keyCode && !currentKeyInfo.keyDown)
    {
        currentGroupInfo.previousKey = 0;
    }

    if (currentKeyInfo.keyDown)
    {
        currentKeyInfo.keyDown = false;
        if (currentGroupInfo.activeKey == keyCode && currentGroupInfo.previousKey != 0)
        {
            SendKey(keyCode, false);
            currentGroupInfo.activeKey = currentGroupInfo.previousKey;
            currentGroupInfo.previousKey = 0;
            SendKey(currentGroupInfo.activeKey, true);
        }
        else
        {
            currentGroupInfo.previousKey = 0;
            if (currentGroupInfo.activeKey == keyCode) currentGroupInfo.activeKey = 0;
            SendKey(keyCode, false);
        }
    }
}

bool isSimulatedKeyEvent(DWORD flags) {
    return flags & 0x10;
}

void SendKey(int targetKey, bool keyDown)
{
    static INPUT input = {0};
    input.ki.wVk = targetKey;
    input.ki.wScan = MapVirtualKey(targetKey, 0);
    input.type = INPUT_KEYBOARD;
    DWORD flags = KEYEVENTF_SCANCODE;
    input.ki.dwFlags = keyDown ? flags : flags | KEYEVENTF_KEYUP;
    addRandomDelay();
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

void UpdateTrayIcon()
{
    const TCHAR* iconFile = TEXT("icon.ico");
    if (isLocked)
    {
        iconFile = TEXT("icon_off.ico");
    }
    else if (vacBypassAEnabled || vacBypassBEnabled)
    {
        iconFile = TEXT("icon_vac_bypass.ico");
    }

    HICON hIcon = (HICON)LoadImage(NULL, iconFile, IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    if (!hIcon)
    {
        hIcon = (HICON)LoadImage(NULL, TEXT("icon.ico"), IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
    }

    if (hIcon)
    {
        nid.hIcon = hIcon;
        Shell_NotifyIcon(NIM_MODIFY, &nid);
        DestroyIcon(hIcon);
    }
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

            // context menu
            HMENU hMenu = CreatePopupMenu();
            // settings & tweaks
            AppendMenu(hMenu, MF_STRING, ID_TRAY_REBIND_KEYS, TEXT("Rebind Keys"));
            AppendMenu(hMenu, MF_STRING, ID_TRAY_RESTART_SNAPKEY, TEXT("Restart SnapKey"));
            AppendMenu(hMenu, MF_STRING, ID_TRAY_LOCK_FUNCTION, isLocked ? TEXT("Enable SnapKey") : TEXT("Disable SnapKey")); // dynamicly switch between state
            AppendMenu(hMenu, MF_STRING | (vacBypassAEnabled ? MF_CHECKED : 0), ID_TRAY_VAC_BYPASS_A, TEXT("VAC bypass A"));
            AppendMenu(hMenu, MF_STRING | (vacBypassBEnabled ? MF_CHECKED : 0), ID_TRAY_VAC_BYPASS_B, TEXT("VAC bypass B"));
            // support & info
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_TRAY_HELP, TEXT("Get Help"));
            AppendMenu(hMenu, MF_STRING, ID_TRAY_CHECKUPDATE, TEXT("Check Updates"));
            AppendMenu(hMenu, MF_STRING, ID_TRAY_VERSION_INFO, TEXT("Version Info (1.2.8)"));
            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            // exit
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
            UpdateTrayIcon();
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_TRAY_EXIT_CONTEXT_MENU_ITEM: // context menu options
            PostQuitMessage(0);
            break;
        case ID_TRAY_VERSION_INFO: // get version info
            {
                std::string versionInfo = GetVersionInfo();
                MessageBox(hwnd, versionInfo.c_str(), TEXT("SnapKey Version Info"), MB_OK);
            }
            break;
        case ID_TRAY_REBIND_KEYS: // rebind keys - open cfg
            {
                TCHAR szExeFileName[MAX_PATH];
                GetModuleFileName(NULL, szExeFileName, MAX_PATH);
                std::string exePath = std::string(szExeFileName);
                std::string exeDir = exePath.substr(0, exePath.find_last_of("\\/"));
                std::string configPath = exeDir + "\\config.cfg";
                ShellExecute(NULL, TEXT("open"), configPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }
            break;
        
        case ID_TRAY_HELP: // get help - open README.pdf
            {
                ShellExecute(NULL, TEXT("open"), TEXT("README.pdf"), NULL, NULL, SW_SHOWNORMAL);
            }
            break;

        case ID_TRAY_CHECKUPDATE: // check updates - open github release page
            {
                int result = MessageBox(NULL,
                            TEXT("You are about to visit the SnapKey update page (github.com). Continue?"),
                            TEXT("Update SnapKey"),
                            MB_YESNO | MB_ICONQUESTION);

                        if (result == IDYES)
                    {
                ShellExecute(NULL, TEXT("open"), TEXT("https://github.com/cafali/SnapKey/releases"), NULL, NULL, SW_SHOWNORMAL);
                }
            }
            break;

        case ID_TRAY_RESTART_SNAPKEY: // restart snapkey 
            {
                TCHAR szExeFileName[MAX_PATH];
                GetModuleFileName(NULL, szExeFileName, MAX_PATH);
                ShellExecute(NULL, NULL, szExeFileName, NULL, NULL, SW_SHOWNORMAL);
                PostQuitMessage(0);
            }
            break;
        case ID_TRAY_LOCK_FUNCTION: // lock sticky keys
            {
                isLocked = !isLocked;
                UpdateTrayIcon();
            }
            break;
        case ID_TRAY_VAC_BYPASS_A: // toggle VAC bypass A
            {
                vacBypassAEnabled = !vacBypassAEnabled;
                WriteConfigValue("config.cfg", "vac_bypass_a", vacBypassAEnabled ? 1 : 0);
                UpdateTrayIcon();
            }
            break;
        case ID_TRAY_VAC_BYPASS_B: // toggle VAC bypass B
            {
                vacBypassBEnabled = !vacBypassBEnabled;
                WriteConfigValue("config.cfg", "vac_bypass_b", vacBypassBEnabled ? 1 : 0);
                UpdateTrayIcon();
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

// version information window
std::string GetVersionInfo() {
    return "SnapKey v1.2.8 (R17)\n"
           "Version Date: June 19, 2025\n"
           "Repository: github.com/cafali/SnapKey\n"
           "License: MIT License\n";
}

// Function to copy snapkey.backup (meta folder) to the main directory
void RestoreConfigFromBackup(const std::string& backupFilename, const std::string& destinationFilename)
{
    // Get the executable path
    char exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);
    std::string exeDir = std::string(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
    
    std::string sourcePath = exeDir + "\\meta\\" + backupFilename;
    std::string destinationPath = exeDir + "\\" + destinationFilename;

    if (CopyFile(sourcePath.c_str(), destinationPath.c_str(), FALSE)) {
        // Copy successful
        MessageBox(NULL, TEXT("Default config restored from backup successfully."), TEXT("SnapKey"), MB_ICONINFORMATION | MB_OK);
    } else {
        // backup.snapkey copy failed
        DWORD error = GetLastError();
        std::string errorMsg = "Failed to restore config from backup. Error code: " + std::to_string(error);
        MessageBox(NULL, errorMsg.c_str(), TEXT("SnapKey Error"), MB_ICONERROR | MB_OK);
    }
}

// Restore config.cfg from backup.snapkey
void CreateDefaultConfig(const std::string& filename)
{
    std::string backupFilename = "backup.snapkey";
    // Just pass the filenames, as RestoreConfigFromBackup now handles the full paths
    RestoreConfigFromBackup(backupFilename, filename);
}

void WriteConfigValue(const std::string& filename, const std::string& key, int value)
{
    std::ifstream inFile(filename);
    std::vector<std::string> lines;
    bool found = false;
    if (inFile.is_open()) {
        std::string line;
        std::regex pat("^\\s*" + key + "\\s*=");
        while (getline(inFile, line)) {
            if (std::regex_search(line, pat)) {
                lines.push_back(key + " = " + std::to_string(value));
                found = true;
            } else {
                lines.push_back(line);
            }
        }
        inFile.close();
    }

    if (!found) {
        lines.push_back(key + " = " + std::to_string(value));
    }

    std::ofstream outFile(filename, std::ios::trunc);
    for (const auto& l : lines) {
        outFile << l << "\n";
    }
}

// Check for config.cfg
bool LoadConfig(const std::string& filename)
{
    // Get the executable path
    char exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);
    std::string exeDir = std::string(exePath);
    exeDir = exeDir.substr(0, exeDir.find_last_of("\\/"));
    
    std::string fullPath = exeDir + "\\" + filename;
    
    std::ifstream configFile(fullPath);
    if (!configFile.is_open()) {
        CreateDefaultConfig(filename);  // Restore config from backup.snapkey if file doesn't exist
        return false;
    }

    string line; // Check for duplicated keys in the config file
    int id = 0;
    bool foundA = false, foundB = false;
    bool foundAMin = false, foundAMax = false;
    bool foundBMin = false, foundBMax = false;

    while (getline(configFile, line)) {
        istringstream iss(line);
        string key;
        int value;
        regex secPat(R"(\s*\[Group\]\s*)");
        if (regex_match(line, secPat))
        {
            id++;
        }
        else if (getline(iss, key, '=') && (iss >> value))
        {
            key = regex_replace(key, regex("^\\s+|\\s+$"), "");
            if (key.find("key") != string::npos)
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
            else if (key == "vac_bypass_a") {
                vacBypassAEnabled = (value != 0); foundA = true;
            }
            else if (key == "vac_bypass_b") {
                vacBypassBEnabled = (value != 0); foundB = true;
            }
            else if (key == "vac_a_min_delay") {
                vacAMinDelay = value; foundAMin = true;
            }
            else if (key == "vac_a_max_delay") {
                vacAMaxDelay = value; foundAMax = true;
            }
            else if (key == "vac_b_min_delay") {
                vacBMinDelay = value; foundBMin = true;
            }
            else if (key == "vac_b_max_delay") {
                vacBMaxDelay = value; foundBMax = true;
            }
        }
        if (line.find("random_delay_ms=") == 0)
        {
            std::smatch match;
            std::regex delayPattern(R"(\s*(\d+)\s*,\s*(\d+)\s*)");
            if (std::regex_search(line, match, delayPattern) && match.size()) {
                minDelay = std::stoi(match[1].str());
                maxDelay = std::stoi(match[2].str());
            }
        }
    }
    configFile.close();

    if (vacAMinDelay < 0 || vacAMaxDelay < 0 || vacAMaxDelay < vacAMinDelay) {
        vacAMinDelay = 15;
        vacAMaxDelay = 35;
    }
    if (vacBMinDelay < 0 || vacBMaxDelay < 0 || vacBMaxDelay < vacBMinDelay) {
        vacBMinDelay = 5;
        vacBMaxDelay = 15;
    }

    if (!foundA) WriteConfigValue(filename, "vac_bypass_a", vacBypassAEnabled ? 1 : 0);
    if (!foundB) WriteConfigValue(filename, "vac_bypass_b", vacBypassBEnabled ? 1 : 0);
    if (!foundAMin) WriteConfigValue(filename, "vac_a_min_delay", vacAMinDelay);
    if (!foundAMax) WriteConfigValue(filename, "vac_a_max_delay", vacAMaxDelay);
    if (!foundBMin) WriteConfigValue(filename, "vac_b_min_delay", vacBMinDelay);
    if (!foundBMax) WriteConfigValue(filename, "vac_b_max_delay", vacBMaxDelay);

    return true;
}
