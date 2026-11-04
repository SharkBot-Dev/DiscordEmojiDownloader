#include <Windows.h>
#include <curl/curl.h>
#include <string>
#include <regex>
#include <optional>
#include <ranges>
#include <string_view>
#include <iostream>

std::optional<std::string> getEmojiUrl(const std::string& emoji)
{
    static const std::regex pattern(
        R"(<(a?):[a-zA-Z0-9_]+:([0-9]+)>)"
    );

    std::smatch match;

    if (!std::regex_match(emoji, match, pattern))
        return std::nullopt;

    const std::string ext = match[1].matched && !match[1].str().empty()
        ? "gif"
        : "png";

    return "https://cdn.discordapp.com/emojis/" +
        match[2].str() + "." + ext;
}

std::optional<std::string> getEmojiFileName(const std::string& emoji)
{
    static const std::regex pattern(
        R"(<(a?):[a-zA-Z0-9_]+:([0-9]+)>)"
    );

    std::smatch match;

    if (!std::regex_match(emoji, match, pattern))
        return std::nullopt;

    const std::string ext = match[1].matched && !match[1].str().empty()
        ? "gif"
        : "png";

    return match[2].str() + "." + ext;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

BOOL RegisterApp(const char* windowID, WNDPROC windowProc) {
	WNDCLASSEXA wc;
	wc.cbSize = sizeof(WNDCLASSEXA);
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = windowProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = GetModuleHandleA(NULL);
	wc.hIcon = (HICON)LoadIconA(NULL, MAKEINTRESOURCEA(32512));
	wc.hCursor = (HCURSOR)LoadCursorA(NULL, MAKEINTRESOURCEA(32512));
	wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	wc.lpszMenuName = NULL;
	wc.lpszClassName = windowID;
	wc.hIconSm = (HICON)LoadImageA(NULL, MAKEINTRESOURCEA(32512), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
	return (RegisterClassExA(&wc));
}

static size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

void downloadEmoji(const char* url, const char* filename) {
    CURL *curl;
    CURLcode res;
    FILE *fp;
    const char *outfilename = filename;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if(curl) {
        fp = fopen(outfilename, "wb");
        if(!fp) {
            fprintf(stderr, "File open error\n");
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n",
                    curl_easy_strerror(res));
        }

        fclose(fp);
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    return;
}

HWND hEdit;

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CLOSE:
            DestroyWindow(hWnd);
            break;
            
        case WM_DESTROY:
            PostQuitMessage(0);
            break;

        case WM_CREATE: {
            CreateWindowEx(
                0,                          // 拡張スタイル
                TEXT("STATIC"),                   // クラス名（固定で "STATIC"）
                TEXT("Please enter an emoji in the box below."),         // 表示する文字列
                WS_CHILD | WS_VISIBLE | SS_LEFT, // ウィンドウのスタイル
                10, 10, 450, 25,           // X座標, Y座標, 幅, 高さ
                hWnd,                       // 親ウィンドウのハンドル
                (HMENU)101,                 // コントロールID
                ((LPCREATESTRUCT)lParam)->hInstance, // インスタンスハンドル
                NULL
            );

            CreateWindowEx(
                0,                          // 拡張スタイル
                TEXT("STATIC"),                   // クラス名（固定で "STATIC"）
                TEXT("You can separate multiple emojis with line breaks."),         // 表示する文字列
                WS_CHILD | WS_VISIBLE | SS_LEFT, // ウィンドウのスタイル
                10, 30, 450, 25,           // X座標, Y座標, 幅, 高さ
                hWnd,                       // 親ウィンドウのハンドル
                (HMENU)101,                 // コントロールID
                ((LPCREATESTRUCT)lParam)->hInstance, // インスタンスハンドル
                NULL
            );

            hEdit = CreateWindowEx(
                WS_EX_CLIENTEDGE,          // 拡張スタイル（立体的な枠線など）
                TEXT("EDIT"),              // 定義済みコントロールクラス名
                TEXT(""),      // 初期表示する文字列
                WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL | ES_MULTILINE, 
                10, 60,                    // X座標, Y座標
                450, 120,                   // 幅, 高さ
                hWnd,                // 親ウィンドウのハンドル
                (HMENU)1,         // コントロールID
                GetModuleHandleA(NULL),                 // インスタンスハンドル
                NULL
            );

            CreateWindowEx(
                0,                      // 拡張スタイル
                TEXT("BUTTON"),              // クラス名（ボタンコントロール）
                TEXT("Download!"),       // ウィンドウ名（ボタンのテキスト）
                WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, // ウィンドウスタイル（★WS_CHILDが必要）
                10, 190, 100, 30,        // 位置 (x, y) と サイズ (幅, 高さ)
                hWnd,                   // 親ウィンドウのハンドル
                (HMENU)2,
                GetModuleHandleA(NULL),              // インスタンスハンドル
                NULL                    // 拡張データ
            );

            break;
        }

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);
            
            switch (wmId)
            {
                case 2: {
                    char emojiText[256];

                    SendMessageA(
                        hEdit,
                        WM_GETTEXT,
                        sizeof(emojiText),
                        (LPARAM)emojiText
                    );

                    if (emojiText == NULL || emojiText[0] == '\0') {
                        MessageBox(
                            NULL, 
                            TEXT("Emoji not found."), 
                            TEXT("Error"), 
                            MB_ICONERROR | MB_ICONINFORMATION
                        );
                        break;
                    }

                    const std::string_view emojiTextView(emojiText);
                    size_t lineStart = 0;
                    int downloadOKEmojis = 0;
                    while (lineStart <= emojiTextView.size()) {
                        const size_t lineEnd = emojiTextView.find('\n', lineStart);
                        const size_t lineLength = lineEnd == std::string_view::npos
                            ? emojiTextView.size() - lineStart
                            : lineEnd - lineStart;
                        std::string emoji(emojiTextView.substr(lineStart, lineLength));
                        if (!emoji.empty() && emoji.back() == '\r')
                            emoji.pop_back();

                        std::string emoji_url = getEmojiUrl(emoji).value_or("");
                        if (emoji_url.empty()) {
                            break;
                        }

                        downloadEmoji(emoji_url.c_str(), getEmojiFileName(emoji).value_or("").c_str());

                        if (lineEnd == std::string_view::npos)
                            break;
                        lineStart = lineEnd + 1;
                        downloadOKEmojis = downloadOKEmojis + 1;
                    }

                    if (downloadEmoji == 0) {
                        MessageBox(
                            NULL, 
                            TEXT("Emoji not found."), 
                            TEXT("Error"), 
                            MB_ICONERROR | MB_ICONINFORMATION
                        );
                        break;
                    }

                    MessageBox(
                        NULL, 
                        TEXT("I downloaded the emojis."), 
                        TEXT("Success!"), 
                        MB_OK | MB_ICONINFORMATION
                    );

                    break;
                }
            }
            break;
        }
        
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hCurInst, HINSTANCE hPrevInst, LPSTR lpsCmdLine, int nCmdShow) {
    FreeConsole();

	HWND hWnd;

	if (!RegisterApp("EmojiDownloader", WndProc)) return FALSE;

	hWnd = CreateWindowA(
		"EmojiDownloader",
		"EmojiDownloader",
		WS_OVERLAPPEDWINDOW,
		0, 0, 500, 300,
		NULL, NULL, hCurInst, NULL);

	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
	return 0;
}