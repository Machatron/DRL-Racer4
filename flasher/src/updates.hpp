#pragma once
#include "core.hpp"
#include "release.h"
#include <array>
#include <commctrl.h>
#include <shlobj.h>
#include <winhttp.h>

namespace drl
{
inline std::array<unsigned, 3> releaseVersion(std::string text)
{
    if (!text.empty() && text.front() == 'v')
        text.erase(0, 1);
    std::array<unsigned, 3> version{};
    std::istringstream input(text);
    for (unsigned i = 0; i < 3; ++i)
    {
        require(input.peek() >= '0' && input.peek() <= '9' && bool(input >> version[i]),
                "Invalid release version.");
        if (i < 2)
            require(input.get() == '.', "Invalid release version.");
    }
    require(input.peek() == EOF, "Invalid release version.");
    return version;
}

inline Json latestRelease(const fs::path &folder)
{
#if DRL_UPDATE_USE_GH
    PWSTR programs = nullptr;
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programs)),
            "Cannot find GitHub CLI.");
    auto gh = fs::path(programs) / L"GitHub CLI/gh.exe";
    CoTaskMemFree(programs);
    auto log = folder / L"update.json";
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE output = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(output != INVALID_HANDLE_VALUE, "Cannot read release information.");
    std::wstring repository = wide(DRL_UPDATE_REPOSITORY);
    std::wstring command =
        L"\"" + gh.wstring() + L"\" api repos/" + repository + L"/releases/latest";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = output;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION child{};
    auto started =
        CreateProcessW(gh.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, gh.parent_path().c_str(), &startup, &child);
    CloseHandle(output);
    require(started, "Cannot start GitHub CLI.");
    CloseHandle(child.hThread);
    auto waited = WaitForSingleObject(child.hProcess, 10000);
    if (waited != WAIT_OBJECT_0)
        TerminateProcess(child.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    require(waited == WAIT_OBJECT_0 && code == 0, "Release check failed.");
    return jsonRead(log);
#else
    auto close = [](void *handle) {
        if (handle)
            WinHttpCloseHandle(handle);
    };
    using Internet = std::unique_ptr<void, decltype(close)>;
    Internet session(WinHttpOpen(L"DRL-Racer4/" DRL_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
                     close);
    require(bool(session), "Cannot open update connection.");
    WinHttpSetTimeouts(session.get(), 3000, 3000, 3000, 3000);
    Internet connection(
        WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0), close);
    require(bool(connection), "Cannot connect to GitHub.");
    auto path = L"/repos/" + wide(DRL_UPDATE_REPOSITORY) + L"/releases/latest";
    Internet request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE),
                     close);
    require(bool(request), "Cannot request release information.");
    require(WinHttpSendRequest(request.get(), L"Accept: application/vnd.github+json\r\n", -1,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request.get(), nullptr),
            "Release check failed.");
    DWORD status = 0, size = sizeof(status);
    require(WinHttpQueryHeaders(
                request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) &&
                status == 200,
            "Release information is unavailable.");
    std::string body;
    char buffer[4096];
    DWORD count = 0;
    auto deadline = GetTickCount64() + 10000;
    do
    {
        require(GetTickCount64() < deadline &&
                    WinHttpReadData(request.get(), buffer, sizeof(buffer), &count),
                "Release check timed out.");
        body.append(buffer, count);
        require(body.size() <= 262144, "Release response is too large.");
    } while (count);
    return Json::parse(body);
#endif
}

inline bool updateAvailable(const fs::path &folder)
{
    try
    {
        auto release = latestRelease(folder);
        return !release.value("draft", false) && !release.value("prerelease", false) &&
               releaseVersion(release.at("tag_name")) > releaseVersion(DRL_VERSION);
    }
    catch (...)
    {
        return false;
    }
}

inline void showUpdateNotice(HWND parent)
{
    struct Countdown
    {
        ULONGLONG started = 0;
        bool complete = false;
        HWND close = nullptr;
    } countdown;
    TASKDIALOG_BUTTON buttons[] = {{1001, L"Go to Release Website"}, {IDCANCEL, L"Close"}};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = parent;
    config.dwFlags =
        TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = L"DRL Flasher";
    config.pszContent = L"You are using outdated firmware and tools for the DRL Racer4";
    config.cButtons = 2;
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
    config.cxWidth = 190;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&countdown);
    config.pfCallback = [](HWND dialog, UINT event, WPARAM button, LPARAM,
                           LONG_PTR data) -> HRESULT {
        auto &state = *reinterpret_cast<Countdown *>(data);
        if (event == TDN_CREATED)
            EnumChildWindows(
                dialog,
                [](HWND child, LPARAM data) -> BOOL {
                    wchar_t label[32], type[32];
                    GetWindowTextW(child, label, 32);
                    GetClassNameW(child, type, 32);
                    if (wcscmp(type, L"Button") == 0 && wcscmp(label, L"Close") == 0)
                        reinterpret_cast<Countdown *>(data)->close = child;
                    return TRUE;
                },
                data);
        if (event == TDN_BUTTON_CLICKED && button == IDCANCEL && !state.complete)
        {
            if (!state.started)
            {
                state.started = GetTickCount64();
                SetWindowTextW(state.close, L"5");
            }
            return S_FALSE;
        }
        if (event == TDN_TIMER && state.started)
        {
            auto elapsed = GetTickCount64() - state.started;
            if (elapsed >= 5000)
            {
                state.complete = true;
                SendMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
            }
            else
                SetWindowTextW(state.close, std::to_wstring(5 - elapsed / 1000).c_str());
        }
        return S_OK;
    };
    int button = 0;
    if (SUCCEEDED(TaskDialogIndirect(&config, &button, nullptr, nullptr)) && button == 1001)
        ShellExecuteW(parent, L"open", L"https://github.com/Open-DRL-Project/DRL-Racer4/releases",
                      nullptr, nullptr, SW_SHOWNORMAL);
}
}
