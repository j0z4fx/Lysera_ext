#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace {
    constexpr wchar_t kHost[] = L"raw.githubusercontent.com";
    constexpr wchar_t kRepoRoot[] = L"/j0z4fx/Lysera_ext/main/release/";
    constexpr wchar_t kWindowClass[] = L"LyseraUpdaterWindow";

    HWND g_window = nullptr;
    std::wstring g_status = L"Preparing update...";
    float g_progress = 0.0f;

    const std::array<std::wstring, 8> kFiles = {
        L"Lysera.exe",
        L"assets/fonts/Inter-Medium.ttf",
        L"assets/fonts/Inter-Regular.ttf",
        L"assets/fonts/Inter-SemiBold.ttf",
        L"assets/icons/eye.svg",
        L"assets/icons/mouse.svg",
        L"assets/icons/settings.svg",
        L"assets/icons/target.svg"
    };

    std::string narrow_path(const std::wstring& value) {
        return fs::path(value).generic_string();
    }

    void pump_messages() {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    void set_status(std::wstring status, float progress) {
        g_status = std::move(status);
        g_progress = std::clamp(progress, 0.0f, 1.0f);
        if (g_window) {
            InvalidateRect(g_window, nullptr, FALSE);
            UpdateWindow(g_window);
            pump_messages();
        }
    }

    std::vector<unsigned char> http_get(const std::wstring& path) {
        HINTERNET session = WinHttpOpen(L"LyseraLoader/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) throw std::runtime_error("WinHttpOpen failed");

        WinHttpSetTimeouts(session, 5000, 5000, 15000, 15000);
        HINTERNET connection = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection) {
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpConnect failed");
        }

        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("WinHttpOpenRequest failed");
        }

        const wchar_t headers[] = L"Cache-Control: no-cache\r\nPragma: no-cache\r\n";
        BOOL ok = WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (ok) {
            ok = WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
        }
        if (!ok || status != 200) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("Download returned HTTP " + std::to_string(status));
        }

        std::vector<unsigned char> body;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            const std::size_t offset = body.size();
            body.resize(offset + available);
            DWORD received = 0;
            if (!WinHttpReadData(request, body.data() + offset, available, &received)) {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                throw std::runtime_error("Download read failed");
            }
            body.resize(offset + received);
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return body;
    }

    std::string sha256(const std::vector<unsigned char>& bytes) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD object_size = 0;
        DWORD hash_size = 0;
        DWORD result_size = 0;

        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0) < 0 ||
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &result_size, 0) < 0) {
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("SHA-256 initialization failed");
        }

        std::vector<unsigned char> object(object_size);
        std::vector<unsigned char> digest(hash_size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0 ||
            BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) < 0 ||
            BCryptFinishHash(hash, digest.data(), hash_size, 0) < 0) {
            if (hash) BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("SHA-256 calculation failed");
        }

        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        std::ostringstream output;
        output << std::hex << std::setfill('0');
        for (unsigned char byte : digest) output << std::setw(2) << static_cast<int>(byte);
        return output.str();
    }

    std::string trim(std::string value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
        std::size_t first = 0;
        while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
        return value.substr(first);
    }

    std::unordered_map<std::string, std::string> parse_checksums(const std::string& text) {
        std::unordered_map<std::string, std::string> checksums;
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line);
            std::string digest;
            std::string path;
            fields >> digest >> path;
            if (!digest.empty() && !path.empty()) checksums[path] = digest;
        }
        return checksums;
    }

    std::string read_text(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return {};
        return { std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
    }

    void write_atomic(const fs::path& destination, const std::vector<unsigned char>& bytes) {
        fs::create_directories(destination.parent_path());
        fs::path temporary = destination;
        temporary += L".download";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("Unable to create update file");
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!output) throw std::runtime_error("Unable to write update file");
        }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            throw std::runtime_error("Unable to replace a running or locked Lysera file");
        }
    }

    fs::path executable_directory() {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return fs::path(path).parent_path();
    }

    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCHITTEST) return HTCAPTION;
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT bounds{};
            GetClientRect(window, &bounds);
            HBRUSH background = CreateSolidBrush(RGB(20, 19, 23));
            FillRect(dc, &bounds, background);
            DeleteObject(background);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(245, 242, 247));
            HFONT title_font = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Segoe UI");
            HFONT old_font = static_cast<HFONT>(SelectObject(dc, title_font));
            TextOutW(dc, 28, 24, L"Lysera", 6);
            SelectObject(dc, old_font);
            DeleteObject(title_font);

            SetTextColor(dc, RGB(171, 167, 179));
            HFONT body_font = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH, L"Segoe UI");
            old_font = static_cast<HFONT>(SelectObject(dc, body_font));
            TextOutW(dc, 29, 63, g_status.c_str(), static_cast<int>(g_status.size()));

            RECT track{ 29, 99, bounds.right - 29, 106 };
            HBRUSH track_brush = CreateSolidBrush(RGB(47, 44, 53));
            FillRect(dc, &track, track_brush);
            DeleteObject(track_brush);
            RECT fill = track;
            fill.right = fill.left + static_cast<LONG>((track.right - track.left) * g_progress);
            HBRUSH fill_brush = CreateSolidBrush(RGB(253, 138, 207));
            FillRect(dc, &fill, fill_brush);
            DeleteObject(fill_brush);

            SelectObject(dc, old_font);
            DeleteObject(body_font);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void create_window(HINSTANCE instance) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kWindowClass;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&window_class);

        constexpr int width = 430;
        constexpr int height = 142;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        g_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"Lysera Loader",
            WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
        if (!g_window) throw std::runtime_error("Unable to create loader window");
        SetWindowRgn(g_window, CreateRoundRectRgn(0, 0, width + 1, height + 1, 22, 22), TRUE);
        ShowWindow(g_window, SW_SHOW);
        UpdateWindow(g_window);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
    try {
        create_window(instance);
        const fs::path root = executable_directory();
        const fs::path version_cache = root / L"data" / L"cache" / L"loader.version";
        fs::create_directories(version_cache.parent_path());

        set_status(L"Checking for updates...", 0.08f);
        const std::wstring cache_buster = L"?v=" + std::to_wstring(GetTickCount64());
        const auto version_bytes = http_get(std::wstring(kRepoRoot) + L"version.txt" + cache_buster);
        const std::string remote_version = trim({ version_bytes.begin(), version_bytes.end() });
        if (remote_version.empty()) throw std::runtime_error("Published version is empty");

        bool needs_update = trim(read_text(version_cache)) != remote_version;
        for (const auto& file : kFiles) needs_update = needs_update || !fs::exists(root / fs::path(file));

        if (needs_update) {
            set_status(L"Reading release manifest...", 0.14f);
            const auto checksum_bytes = http_get(std::wstring(kRepoRoot) + L"checksums.txt" + cache_buster);
            const auto checksums = parse_checksums({ checksum_bytes.begin(), checksum_bytes.end() });

            for (std::size_t index = 0; index < kFiles.size(); ++index) {
                const std::wstring& relative = kFiles[index];
                const std::string generic = narrow_path(relative);
                const auto expected = checksums.find(generic);
                if (expected == checksums.end()) throw std::runtime_error("Release checksum is missing");

                set_status(L"Updating " + fs::path(relative).filename().wstring() + L"...",
                    0.18f + 0.70f * static_cast<float>(index) / static_cast<float>(kFiles.size()));
                std::wstring remote_path = std::wstring(kRepoRoot) + relative;
                std::replace(remote_path.begin(), remote_path.end(), L'\\', L'/');
                const auto bytes = http_get(remote_path + cache_buster);
                std::string actual = sha256(bytes);
                std::transform(actual.begin(), actual.end(), actual.begin(),
                    [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                std::string wanted = expected->second;
                std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                    [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
                if (actual != wanted) throw std::runtime_error("Downloaded file failed SHA-256 verification");
                write_atomic(root / fs::path(relative), bytes);
            }

            const std::vector<unsigned char> version_output(remote_version.begin(), remote_version.end());
            write_atomic(version_cache, version_output);
        }

        const bool check_only = command_line && wcsstr(command_line, L"--check-only");
        if (check_only) {
            set_status(needs_update ? L"Update verification complete." : L"Release verification complete.", 1.0f);
            Sleep(350);
            return 0;
        }

        set_status(needs_update ? L"Update complete. Launching..." : L"Up to date. Launching...", 1.0f);
        const fs::path application = root / L"Lysera.exe";
        const HINSTANCE launched = ShellExecuteW(nullptr, L"runas", application.c_str(), nullptr,
            root.c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) <= 32)
            throw std::runtime_error("Lysera could not be launched");
        Sleep(350);
        return 0;
    }
    catch (const std::exception& error) {
        std::string message = std::string("Lysera update failed:\n\n") + error.what();
        MessageBoxA(g_window, message.c_str(), "Lysera Loader", MB_OK | MB_ICONERROR);
        return 1;
    }
}
