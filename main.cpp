#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr int ID_INPUT = 101;
constexpr int ID_COOLDOWN = 102;
constexpr int ID_PARALLEL = 103;
constexpr int ID_DOWNLOAD = 104;
constexpr int ID_STATUS = 105;
constexpr int ID_LANGUAGE = 106;
constexpr int ID_FOLDER = 107;
constexpr UINT WM_DOWNLOAD_DONE = WM_APP + 1;

struct Job {
    std::string url;
    std::wstring filename;
};

HWND g_input{};
HWND g_cooldown{};
HWND g_parallel{};
HWND g_button{};
HWND g_status{};
HWND g_window{};
std::vector<std::thread> g_workers;
std::atomic<int> g_finished{0};
std::atomic<int> g_succeeded{0};
int g_total = 0;
bool g_japanese = false;
HWND g_title{};
HWND g_description{};
HWND g_codesLabel{};
HWND g_cooldownLabel{};
HWND g_parallelLabel{};
HWND g_language{};
HWND g_folderButton{};
HWND g_folderPath{};
std::wstring g_outputDirectory;
bool g_darkTheme{};
HBRUSH g_darkBrush{};
HFONT g_titleFont{};
HFONT g_bodyFont{};
HFONT g_labelFont{};
HFONT g_buttonFont{};

void updateLanguage();

bool systemUsesDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    return RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size
    ) == ERROR_SUCCESS && value == 0;
}

void updateTheme(HWND hwnd) {
    g_darkTheme = systemUsesDarkTheme();
    const BOOL darkTitleBar = g_darkTheme;
    DwmSetWindowAttribute(hwnd, 20, &darkTitleBar, sizeof(darkTitleBar));
    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    if (uxtheme) {
        using SetThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
        auto setTheme = reinterpret_cast<SetThemeFn>(GetProcAddress(uxtheme, "SetWindowTheme"));
        if (setTheme) {
            const wchar_t* theme = g_darkTheme ? L"DarkMode_Explorer" : L"Explorer";
            for (HWND control : {g_input, g_cooldown, g_parallel}) {
                if (control) setTheme(control, theme, nullptr);
            }
        }
        FreeLibrary(uxtheme);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
    if (g_button) InvalidateRect(g_button, nullptr, TRUE);
    if (g_language) InvalidateRect(g_language, nullptr, TRUE);
}

HFONT makeFont(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void setFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

std::filesystem::path languageSettingsPath() {
    wchar_t modulePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    return std::filesystem::path(modulePath).parent_path() / L"language.txt";
}

void loadLanguagePreference() {
    FILE* file = nullptr;
    if (_wfopen_s(&file, languageSettingsPath().c_str(), L"rb") == 0 && file) {
        const int first = fgetc(file);
        g_japanese = first == 'j' || first == 'J';
        fclose(file);
    }
}

void saveLanguagePreference() {
    FILE* file = nullptr;
    if (_wfopen_s(&file, languageSettingsPath().c_str(), L"wb") == 0 && file) {
        fputs(g_japanese ? "ja\n" : "en\n", file);
        fclose(file);
    }
}

std::optional<Job> parseEmoji(const std::string& emoji) {
    static const std::regex pattern(R"(<(a?):[a-zA-Z0-9_]+:([0-9]+)>)");
    std::smatch match;
    if (!std::regex_match(emoji, match, pattern)) {
        return std::nullopt;
    }

    const std::string ext = match[1].str().empty() ? "png" : "gif";
    const std::string id = match[2].str();
    return Job{"https://cdn.discordapp.com/emojis/" + id + "." + ext,
        std::wstring(id.begin(), id.end()) + L"." + std::wstring(ext.begin(), ext.end())};
}

size_t writeData(void* data, size_t size, size_t count, void* file) {
    return fwrite(data, size, count, static_cast<FILE*>(file));
}

bool download(const Job& job) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, job.filename.c_str(), L"wb") != 0 || !file) {
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(file);
        DeleteFileW(job.filename.c_str());
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DiscordEmojiDownloader/1.0");
    const CURLcode result = curl_easy_perform(curl);
    fclose(file);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) DeleteFileW(job.filename.c_str());
    return result == CURLE_OK;
}

void setStatus(const char* value) {
    SetWindowTextA(g_status, value);
}

void startDownloads() {
    const int length = GetWindowTextLengthA(g_input);
    if (length <= 0) {
        MessageBoxA(
            g_window,
            "Paste one or more emoji codes first.",
            "No emojis",
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }

    std::string text(length + 1, '\0');
    GetWindowTextA(g_input, text.data(), length + 1);
    text.resize(length);
    std::vector<Job> jobs;
    static const std::regex emojiPattern(R"(<(a?):[a-zA-Z0-9_]+:([0-9]+)>)");
    for (std::sregex_iterator it(text.begin(), text.end(), emojiPattern), end; it != end; ++it) {
        if (auto job = parseEmoji(it->str())) {
            jobs.push_back(*job);
        }
    }
    if (jobs.empty()) {
        MessageBoxA(
            g_window,
            "Paste one or more emoji codes first.",
            "No emojis",
            MB_OK | MB_ICONINFORMATION
        );
        return;
    }

    const int cooldown = GetDlgItemInt(g_window, ID_COOLDOWN, nullptr, FALSE);
    const int parallel = GetDlgItemInt(g_window, ID_PARALLEL, nullptr, FALSE);
    if (cooldown < 0 || cooldown > 60000 || parallel < 1 || parallel > 32) {
        MessageBoxA(
            g_window,
            "Cooldown must be 0–60000 ms and parallel downloads 1–32.",
            "Invalid settings",
            MB_OK | MB_ICONWARNING
        );
        return;
    }

    for (auto& job : jobs) {
        job.filename = (std::filesystem::path(g_outputDirectory) / job.filename).wstring();
    }

    EnableWindow(g_button, FALSE);
    EnableWindow(g_input, FALSE);
    EnableWindow(g_cooldown, FALSE);
    EnableWindow(g_parallel, FALSE);

    g_total = static_cast<int>(jobs.size());
    g_finished = 0;
    g_succeeded = 0;
    setStatus(("Starting " + std::to_string(g_total) + " downloads…").c_str());
    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto sharedJobs = std::make_shared<std::vector<Job>>(std::move(jobs));
    auto nextJob = std::make_shared<std::atomic<size_t>>(0);
    auto startGate = std::make_shared<std::mutex>();
    auto nextStartTime =
        std::make_shared<std::chrono::steady_clock::time_point>();

    const int count = (std::min)(parallel, g_total);
    for (int i = 0; i < count; ++i) {
        g_workers.emplace_back([=]() {
            for (;;) {
                const size_t index = (*nextJob)++;
                if (index >= sharedJobs->size()) break;

                {
                    std::lock_guard<std::mutex> lock(*startGate);
                    const auto now = std::chrono::steady_clock::now();
                    if (now < *nextStartTime) {
                        std::this_thread::sleep_until(*nextStartTime);
                    }
                    *nextStartTime = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(cooldown);
                }

                if (download((*sharedJobs)[index])) {
                    ++g_succeeded;
                }

                const int done = ++g_finished;
                PostMessageA(g_window, WM_DOWNLOAD_DONE, done, 0);
            }
        });
    }
}

void layout(HWND hwnd) {
    RECT r;
    GetClientRect(hwnd, &r);
    const int width = r.right - r.left;
    MoveWindow(g_input, 24, 124, width - 48, 186, TRUE);
    MoveWindow(g_cooldown, 145, 350, 90, 28, TRUE);
    MoveWindow(g_parallel, 390, 350, 75, 28, TRUE);
    MoveWindow(g_folderButton, 24, 316, 160, 30, TRUE);
    MoveWindow(g_folderPath, 195, 316, width - 219, 30, TRUE);
    MoveWindow(g_button, width - 174, 405, 150, 42, TRUE);
    MoveWindow(g_status, 24, 414, width - 220, 28, TRUE);
    HWND cooldownLabel = GetDlgItem(hwnd, 204);
    HWND parallelLabel = GetDlgItem(hwnd, 205);
    if (cooldownLabel) {
        MoveWindow(cooldownLabel, 24, 354, 115, 24, TRUE);
    }
    if (parallelLabel) {
        MoveWindow(parallelLabel, 265, 354, 112, 24, TRUE);
    }
}

void updateLanguage() {
    SetWindowTextW(g_window, g_japanese ? L"Discord 絵文字ダウンローダー" : L"Discord Emoji Downloader");
    SetWindowTextW(g_title, g_japanese ? L"Discord 絵文字ダウンローダー" : L"Discord Emoji Downloader");
    SetWindowTextW(g_description, g_japanese
        ? L"カスタム絵文字コードを1行ずつ入力してください。\nファイルは現在のフォルダーに保存されます。"
        : L"Paste custom emoji codes below, one per line. Files are saved to the current folder.");
    SetWindowTextW(g_codesLabel, g_japanese ? L"絵文字コード" : L"Emoji Codes");
    SetWindowTextW(g_cooldownLabel, g_japanese ? L"間隔 (ms)" : L"Cooldown (ms)");
    SetWindowTextW(g_parallelLabel, g_japanese ? L"同時DL数" : L"Parallel downloads");
    SetWindowTextW(g_button, g_japanese ? L"ダウンロード" : L"Download");
    SetWindowTextW(g_language, g_japanese ? L"English" : L"日本語");
    SetWindowTextW(g_folderButton, g_japanese ? L"保存先を選択" : L"Choose folder");
    SetWindowTextW(g_folderPath, g_outputDirectory.c_str());
    SetWindowTextW(g_status, g_japanese ? L"準備完了" : L"Ready");
}
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g_window = hwnd;
        g_titleFont = makeFont(26, FW_SEMIBOLD);
        g_bodyFont = makeFont(16, FW_NORMAL);
        g_labelFont = makeFont(16, FW_SEMIBOLD);
        g_buttonFont = makeFont(15, FW_SEMIBOLD);
        auto setControlFont = [](HWND control) { return control; };

        g_title = setControlFont(CreateWindowExW(
            0, L"STATIC", L"Discord Emoji Downloader", WS_CHILD | WS_VISIBLE,
            24, 20, 450, 30, hwnd, (HMENU)201, nullptr, nullptr
        ));
        g_description = setControlFont(CreateWindowExW(
            0, L"STATIC",
            L"Paste custom emoji codes below, one per line. Files are saved to the current folder.",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_EDITCONTROL, 24, 55, 570, 40, hwnd, (HMENU)202,
            nullptr, nullptr
        ));
        g_codesLabel = setControlFont(CreateWindowExW(
            0, L"STATIC", L"Emoji Codes", WS_CHILD | WS_VISIBLE,
            24, 100, 200, 20, hwnd, (HMENU)203, nullptr, nullptr
        ));
        g_input = setControlFont(CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE |
                ES_AUTOVSCROLL | ES_WANTRETURN,
            24, 124, 552, 186, hwnd, (HMENU)ID_INPUT, nullptr, nullptr
        ));
        g_cooldownLabel = setControlFont(CreateWindowExW(
            0, L"STATIC", L"Cooldown (ms)", WS_CHILD | WS_VISIBLE,
            24, 354, 115, 24, hwnd, (HMENU)204, nullptr, nullptr
        ));
        g_cooldown = setControlFont(CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT", "250", WS_CHILD | WS_VISIBLE | ES_NUMBER,
            145, 350, 90, 28, hwnd, (HMENU)ID_COOLDOWN, nullptr, nullptr
        ));
        g_parallelLabel = setControlFont(CreateWindowExW(
            0, L"STATIC", L"Parallel downloads", WS_CHILD | WS_VISIBLE,
            265, 354, 112, 24, hwnd, (HMENU)205, nullptr, nullptr
        ));
        g_parallel = setControlFont(CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT", "3", WS_CHILD | WS_VISIBLE | ES_NUMBER,
            390, 350, 75, 28, hwnd, (HMENU)ID_PARALLEL, nullptr, nullptr
        ));
        g_status = setControlFont(CreateWindowExW(
            0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
            24, 414, 350, 28, hwnd, (HMENU)ID_STATUS, nullptr, nullptr
        ));
        g_folderButton = setControlFont(CreateWindowExW(
            0, L"BUTTON", L"Choose folder", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            24, 316, 160, 30, hwnd, (HMENU)ID_FOLDER, nullptr, nullptr
        ));
        g_folderPath = setControlFont(CreateWindowExW(
            0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            195, 316, 380, 30, hwnd, (HMENU)108, nullptr, nullptr
        ));
        g_button = setControlFont(CreateWindowExA(
            0, "BUTTON", "Download",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            402, 405, 150, 42, hwnd, (HMENU)ID_DOWNLOAD, nullptr, nullptr
        ));
        g_language = setControlFont(CreateWindowExW(
            0, L"BUTTON", L"日本語", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            480, 18, 100, 28, hwnd, (HMENU)ID_LANGUAGE, nullptr, nullptr
        ));
        wchar_t currentDirectory[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, currentDirectory);
        g_outputDirectory = currentDirectory;
        updateLanguage();
        setFont(g_title, g_titleFont);
        setFont(g_description, g_bodyFont);
        setFont(g_codesLabel, g_labelFont);
        setFont(g_input, g_bodyFont);
        setFont(g_cooldownLabel, g_labelFont);
        setFont(g_cooldown, g_bodyFont);
        setFont(g_parallelLabel, g_labelFont);
        setFont(g_parallel, g_bodyFont);
        setFont(g_status, g_bodyFont);
        setFont(g_button, g_buttonFont);
        setFont(g_language, g_buttonFont);
        setFont(g_folderButton, g_buttonFont);
        setFont(g_folderPath, g_bodyFont);
        updateTheme(hwnd);
        return 0;
    }

    case WM_SIZE: layout(hwnd); return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        RECT rect{0, 0, 620, 510};
        const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
        AdjustWindowRectEx(&rect, style, FALSE, exStyle);
        info->ptMinTrackSize.x = info->ptMaxTrackSize.x = rect.right - rect.left;
        info->ptMinTrackSize.y = info->ptMaxTrackSize.y = rect.bottom - rect.top;
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (g_darkTheme) {
            SetTextColor(dc, RGB(240, 240, 240));
            SetBkColor(dc, RGB(32, 32, 32));
            return reinterpret_cast<LRESULT>(g_darkBrush);
        }
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }

    case WM_CTLCOLOREDIT:
    {
        if (g_darkTheme) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(240, 240, 240));
            SetBkColor(dc, RGB(45, 45, 45));
            return reinterpret_cast<LRESULT>(g_darkBrush);
        }
        break;
    }

    case WM_ERASEBKGND:
        if (g_darkTheme) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            FillRect(reinterpret_cast<HDC>(wParam), &rect, g_darkBrush);
            return 1;
        }
        break;

    case WM_DRAWITEM: {
        auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlID == ID_DOWNLOAD || item->CtlID == ID_LANGUAGE || item->CtlID == ID_FOLDER) {
            HDC dc = item->hDC;
            RECT rect = item->rcItem;
            const bool disabled = (item->itemState & ODS_DISABLED) != 0;
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            COLORREF face = g_darkTheme ? (pressed ? RGB(70, 70, 70) : RGB(55, 55, 55)) : GetSysColor(COLOR_BTNFACE);
            COLORREF ink = g_darkTheme ? RGB(245, 245, 245) : GetSysColor(COLOR_BTNTEXT);
            if (disabled) ink = g_darkTheme ? RGB(150, 150, 150) : GetSysColor(COLOR_GRAYTEXT);
            HBRUSH brush = CreateSolidBrush(face);
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
            FrameRect(dc, &rect, GetSysColorBrush(g_darkTheme ? COLOR_GRAYTEXT : COLOR_BTNSHADOW));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ink);
            HFONT font = reinterpret_cast<HFONT>(SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
            HGDIOBJ oldFont = SelectObject(dc, font);
            wchar_t text[80]{};
            GetWindowTextW(item->hwndItem, text, 80);
            DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, oldFont);
            if (item->itemState & ODS_FOCUS) {
                InflateRect(&rect, -4, -4);
                DrawFocusRect(dc, &rect);
            }
            return TRUE;
        }
        break;
    }

    case WM_SETTINGCHANGE:
        updateTheme(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == ID_LANGUAGE) {
            g_japanese = !g_japanese;
            saveLanguagePreference();
            updateLanguage();
            return 0;
        }
        if (LOWORD(wParam) == ID_DOWNLOAD) {
            startDownloads();
            return 0;
        }
        if (LOWORD(wParam) == ID_FOLDER) {
            BROWSEINFOW browse{};
            browse.hwndOwner = hwnd;
            browse.lpszTitle = g_japanese ? L"絵文字の保存先フォルダーを選択" : L"Select the folder for downloaded emojis";
            browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE folder = SHBrowseForFolderW(&browse);
            if (folder) {
                wchar_t selected[MAX_PATH]{};
                if (SHGetPathFromIDListW(folder, selected)) {
                    g_outputDirectory = selected;
                    SetWindowTextW(g_folderPath, g_outputDirectory.c_str());
                }
                CoTaskMemFree(folder);
            }
            return 0;
        }
        break;

    case WM_DOWNLOAD_DONE: {
        const int done = static_cast<int>(wParam);
        if (done >= g_total) {
            for (auto& worker : g_workers) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
            g_workers.clear();
            curl_global_cleanup();

            char summary[128];
            sprintf_s(
                summary,
                "Finished: %d / %d downloaded",
                g_succeeded.load(),
                g_total
            );
            setStatus(summary);
            EnableWindow(g_button, TRUE);
            EnableWindow(g_input, TRUE);
            EnableWindow(g_cooldown, TRUE);
            EnableWindow(g_parallel, TRUE);
        } else {
            char progress[128];
            sprintf_s(
                progress,
                "Downloading… %d / %d complete",
                done,
                g_total
            );
            setStatus(progress);
        }
        return 0;
    }

    case WM_CLOSE:
        if (!g_workers.empty()) {
            MessageBoxA(
                hwnd,
                "Please wait for the active downloads to finish.",
                "Downloads in progress",
                MB_OK | MB_ICONINFORMATION
            );
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_darkBrush) {
            DeleteObject(g_darkBrush);
            g_darkBrush = nullptr;
        }
        for (HFONT font : {g_titleFont, g_bodyFont, g_labelFont, g_buttonFont}) {
            if (font) DeleteObject(font);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    loadLanguagePreference();
    g_darkBrush = CreateSolidBrush(RGB(32, 32, 32));
    INITCOMMONCONTROLSEX controls{
        sizeof(controls),
        ICC_STANDARD_CLASSES
    };
    InitCommonControlsEx(&controls);

    WNDCLASSEXA windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconA(nullptr, IDI_APPLICATION);
    windowClass.lpszClassName = "EmojiDownloader";
    if (!RegisterClassExA(&windowClass)) {
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    HWND window = CreateWindowExA(
        0,
        windowClass.lpszClassName,
        "Discord Emoji Downloader",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        620,
        510,
        nullptr,
        nullptr,
        instance,
        nullptr
    );
    if (!window) {
        if (SUCCEEDED(comResult)) CoUninitialize();
        return 1;
    }

    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageA(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

    if (SUCCEEDED(comResult)) CoUninitialize();
    return static_cast<int>(message.wParam);
}
