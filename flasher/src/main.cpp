#include "platform.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <memory>
using namespace drl;
namespace
{
HWND win, buildBox, boardBox, cpuBox, ecBox, ledBox, ledLabel, cpuLabel, flashButton;
HFONT font, bold, large;
HBRUSH background = CreateSolidBrush(RGB(255, 255, 255));
std::unique_ptr<Gdiplus::Image> header;
IStream *headerStream = nullptr;
int dpi = 96, percent = 0;
enum class Step
{
    Idle,
    Connect,
    Reset,
    Verify,
    Complete
};
Step step = Step::Idle;
bool busy = false, failed = false;
std::wstring caption = L"Flash";
std::thread worker;
std::unique_ptr<Session> session;
std::vector<fs::path> folders;
std::vector<Json> catalogs;
enum
{
    BUILD = 101,
    BOARD,
    CPU,
    LED,
    FLASH,
    REPORTS,
    BROWSE,
    BACK,
    EC_CPU
};
constexpr UINT EVENT = WM_APP + 1, DONE = WM_APP + 2;
struct Update
{
    std::wstring text;
    int progress;
};
int px(int x)
{
    return MulDiv(x, dpi, 96);
}
int selection(HWND h)
{
    return int(SendMessageW(h, CB_GETCURSEL, 0, 0));
}
std::string selectedProfile()
{
    int board = selection(boardBox);
    if (board == 0)
        return selection(cpuBox) == 0 ? "FC_AUTO" : selection(cpuBox) == 1 ? "FC_F7" : "FC_F4";
    if (board == 1)
        return "EC_AUTO";
    return selection(ledBox) == 0 ? "LED_AUTO" : selection(ledBox) == 1 ? "LED_AB" : "LED_C";
}
HWND control(const wchar_t *cls, const wchar_t *text, int x, int y, int w, int h, int id,
             DWORD style = 0)
{
    auto c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w),
                             px(h), win, reinterpret_cast<HMENU>(INT_PTR(id)),
                             GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return c;
}
HWND combo(int x, int y, int w, int id, std::initializer_list<const wchar_t *> items)
{
    auto c =
        control(L"COMBOBOX", L"", x, y, w, 180, id, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL);
    for (auto s : items)
        SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(c, CB_SETCURSEL, 0, 0);
    return c;
}
void saveUI(bool running = true) noexcept
{
    try
    {
        Json j = {{"running", running},
                  {"pid", GetCurrentProcessId()},
                  {"busy", busy},
                  {"step", int(step)},
                  {"message", narrow(caption)},
                  {"updated_utc", stamp()},
                  {"board_index", selection(boardBox)},
                  {"processor_index", selection(selection(boardBox) == 1 ? ecBox : cpuBox)},
                  {"requested_profile", selectedProfile()},
                  {"led_wiring_index", selection(ledBox)}};
        int b = selection(buildBox);
        if (b >= 0 && size_t(b) < catalogs.size())
            j["build"] = catalogs[b]["name"];
        tryJsonWrite(reportRoot() / L"ui.json", j);
    }
    catch (...)
    {
    }
}
void refresh(bool persist = true)
{
    bool idle = !busy && step == Step::Idle;
    for (int id : {BUILD, BOARD, CPU, EC_CPU, LED, BROWSE})
        EnableWindow(GetDlgItem(win, id), idle);
    bool led = selection(boardBox) == 2;
    ShowWindow(ledBox, led ? SW_SHOW : SW_HIDE);
    ShowWindow(ledLabel, led ? SW_SHOW : SW_HIDE);
    ShowWindow(cpuBox, selection(boardBox) == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(ecBox, selection(boardBox) == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(cpuLabel, led ? SW_HIDE : SW_SHOW);
    SetWindowTextW(cpuLabel, selection(boardBox) == 1 ? L"Firmware" : L"Hardware");
    bool compact = step == Step::Idle && !failed;
    MoveWindow(flashButton, px(compact ? 120 : 24), px(compact ? 370 : 344),
               px(compact ? 160 : 352), px(compact ? 44 : 90), TRUE);
    MoveWindow(GetDlgItem(win, REPORTS), px(24), px(454), px(82), px(26), TRUE);
    MoveWindow(GetDlgItem(win, BACK), px(294), px(454), px(82), px(26), TRUE);
    RECT windowSize = {0, 0, px(400), px(500)};
    AdjustWindowRectExForDpi(&windowSize, DWORD(GetWindowLongPtrW(win, GWL_STYLE)), FALSE, 0, dpi);
    SetWindowPos(win, nullptr, 0, 0, windowSize.right - windowSize.left,
                 windowSize.bottom - windowSize.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    EnableWindow(cpuBox, idle && selection(boardBox) == 0);
    EnableWindow(ecBox, idle && selection(boardBox) == 1);
    EnableWindow(flashButton, !busy && !folders.empty());
    ShowWindow(GetDlgItem(win, BACK), !busy && step != Step::Idle ? SW_SHOW : SW_HIDE);
    SetWindowTextW(flashButton, caption.c_str());
    InvalidateRect(flashButton, nullptr, FALSE);
    if (persist)
        saveUI();
}
void prompt()
{
    failed = false;
    switch (step)
    {
    case Step::Idle:
        caption = L"Flash";
        percent = 0;
        break;
    case Step::Connect:
        caption = selection(boardBox) == 0
                      ? L"Connect Betaflight USB\nwith battery power disconnected"
                  : selection(boardBox) == 1
                      ? L"Connect EC USB\nwith battery power disconnected"
                      : L"Connect to LEDC USB JST\nand connect battery power";
        break;
    case Step::Reset:
        caption = L"Double press the RESET button";
        break;
    case Step::Verify:
        caption = selection(boardBox) == 2
                      ? L"Disconnect battery power for 5 seconds\nand reconnect"
                  : session && session->fc ? L"Connect Betaflight USB in bootloader mode"
                                           : L"Double press the RESET button";
        break;
    case Step::Complete:
        caption = selection(boardBox) == 2
                      ? L"Flash complete\nDisconnect battery power for 5 seconds\nand reconnect"
                  : session && session->fc ? L"Flash complete\nReconnect Betaflight USB"
                                           : L"Flash complete\nPress RESET once to start the board";
        percent = 100;
        break;
    }
    refresh();
}
void notify(const std::string &text, int p)
{
    auto update = std::make_unique<Update>(Update{wide(text), p});
    if (PostMessageW(win, EVENT, 0, reinterpret_cast<LPARAM>(update.get())))
        update.release();
}
template <class F> void run(F action, Step next)
{
    busy = true;
    caption = L"Connecting...";
    refresh();
    if (worker.joinable())
        worker.join();
    worker = std::thread(
        [action, next]
        {
            int result = int(next);
            try
            {
                action();
            }
            catch (const std::exception &e)
            {
                try
                {
                    if (session && !session->folder.empty())
                        session->fail(e.what());
                }
                catch (...)
                {
                }
                notify(e.what(), 0);
                result = -1;
            }
            PostMessageW(win, DONE, WPARAM(result), 0);
        });
}
void addBuild(const fs::path &p)
{
    auto j = jsonRead(p / L"bundle.json");
    require(j.at("schema") == 1, "Unsupported build package.");
    for (auto &e : j.at("profiles").items())
        verifiedFile(p, e.value().at("image"));
    folders.push_back(p);
    catalogs.push_back(j);
    auto name = wide(j.at("date").get<std::string>() + "   " + j.at("name").get<std::string>());
    SendMessageW(buildBox, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
}
bool resumePending()
{
    auto root = reportRoot();
    auto report = latestReport(root);
    if (report.is_null())
        return false;
    if (!report.value("write_started", false) || report.value("verified", false))
        return false;
    int b = selection(boardBox);
    auto key = selectedProfile();
    if (!matchesPendingProfile(key, report))
        return false;
    auto folder = fs::u8path(report.at("session").get<std::string>());
    require(fs::weakly_canonical(folder).parent_path() == fs::weakly_canonical(root),
            "Invalid report folder.");
    session = std::make_unique<Session>();
    auto &s = *session;
    s.root = root;
    s.folder = folder;
    s.report = report;
    s.catalog = jsonRead(folder / L"catalog.json");
    s.profile = s.catalog.at("profiles").at(report.at("profile").get<std::string>());
    s.before = read(folder / L"before.bin");
    s.expected = read(folder / L"expected.bin");
    require(hash(s.before) == report.at("before_sha256") &&
                hash(s.expected) == report.at("expected_sha256"),
            "The saved flash report has changed.");
    s.fc = b == 0;
    s.serial = report.value("usb_serial", "");
    s.update = notify;
    step = Step::Verify;
    prompt();
    return true;
}
void flash()
{
    if (step == Step::Idle)
    {
        if (resumePending())
            return;
        step = Step::Connect;
        prompt();
        return;
    }
    if (step == Step::Complete)
    {
        step = Step::Idle;
        session.reset();
        prompt();
        return;
    }
    if (step == Step::Connect && selection(boardBox) == 1)
    {
        step = Step::Reset;
        prompt();
        return;
    }
    if (step == Step::Verify)
    {
        run([] { session->verify(); }, Step::Complete);
        return;
    }
    int n = selection(buildBox), b = selection(boardBox);
    require(n >= 0, "Select a build.");
    auto key = selectedProfile();
    auto path = folders.at(n);
    auto cat = catalogs.at(n);
    session = std::make_unique<Session>();
    session->update = notify;
    session->fc = b == 0;
    run(
        [path, cat, key, b]
        {
            session->start(path, cat, key, b == 0);
            if (b != 1)
            {
                auto devices = ports();
                require(devices.size() <= 1,
                        "Disconnect other USB serial devices, then click to retry.");
                if (devices.size() == 1)
                {
                    notify("Entering bootloader...", 3);
                    enterBootloader(devices[0].name, b == 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1800));
                }
            }
            session->prepare();
            session->flash();
        },
        b == 0 ? Step::Complete : Step::Verify);
}
void browse()
{
    wchar_t file[32768] = {};
    OPENFILENAMEW o{};
    o.lStructSize = sizeof(o);
    o.hwndOwner = win;
    o.lpstrFilter = L"DRL build\0bundle.json\0";
    o.lpstrFile = file;
    o.nMaxFile = 32768;
    o.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&o))
    {
        addBuild(fs::path(file).parent_path());
        SendMessageW(buildBox, CB_SETCURSEL, folders.size() - 1, 0);
        refresh();
    }
}
void loadHeader()
{
    auto r = FindResourceW(nullptr, MAKEINTRESOURCEW(201), RT_RCDATA);
    require(r != nullptr, "Header image missing.");
    auto size = SizeofResource(nullptr, r);
    auto source = LockResource(LoadResource(nullptr, r));
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    require(memory != nullptr, "Cannot load header.");
    auto target = GlobalLock(memory);
    memcpy(target, source, size);
    GlobalUnlock(memory);
    require(SUCCEEDED(CreateStreamOnHGlobal(memory, TRUE, &headerStream)),
            "Cannot load header stream.");
    header.reset(Gdiplus::Image::FromStream(headerStream));
}
void draw(HDC dc, const std::wstring &s, RECT r, HFONT f, COLORREF color, UINT flags)
{
    SelectObject(dc, f);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), -1, &r, flags);
}
LRESULT CALLBACK procedure(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    try
    {
        switch (msg)
        {
        case WM_CREATE:
        {
            win = h;
            dpi = GetDpiForWindow(h);
            auto makeFont = [](int size, int weight)
            {
                return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
            };
            font = makeFont(13, FW_NORMAL);
            bold = makeFont(14, FW_SEMIBOLD);
            large = makeFont(16, FW_SEMIBOLD);
            loadHeader();
            control(L"STATIC", L"Build", 24, 125, 64, 24, 0);
            buildBox = combo(24, 153, 352, BUILD, {});
            control(L"BUTTON", L"Browse...", 284, 120, 92, 26, BROWSE, WS_TABSTOP);
            control(L"STATIC", L"Board", 24, 199, 64, 24, 0);
            boardBox = combo(24, 227, 352, BOARD, {L"Flight controller", L"EC", L"LEDC"});
            cpuLabel = control(L"STATIC", L"Hardware", 24, 273, 100, 24, 0);
            cpuBox = combo(24, 301, 352, CPU, {L"Detect", L"F7 / RACER4S", L"F4 / RACER4F4"});
            ecBox = combo(24, 301, 352, EC_CPU, {L"Detect"});
            ledLabel = control(L"STATIC", L"Wiring", 24, 273, 64, 24, 0);
            ledBox = combo(24, 301, 352, LED, {L"Detect", L"A / B", L"C"});
            flashButton = control(L"BUTTON", L"Flash", 120, 370, 160, 44, FLASH,
                                  WS_TABSTOP | BS_OWNERDRAW | BS_MULTILINE);
            control(L"BUTTON", L"Reports", 24, 454, 82, 26, REPORTS, WS_TABSTOP);
            control(L"BUTTON", L"Back", 294, 454, 82, 26, BACK, WS_TABSTOP);
            auto root = exeDir() / L"builds";
            if (fs::exists(root))
            {
                std::vector<fs::path> list;
                for (auto &e : fs::directory_iterator(root))
                    if (e.is_directory() && fs::exists(e.path() / L"bundle.json"))
                        list.push_back(e.path());
                std::sort(list.rbegin(), list.rend());
                for (auto &p : list)
                    addBuild(p);
            }
            if (!folders.empty())
                SendMessageW(buildBox, CB_SETCURSEL, 0, 0);
            prompt();
            return 0;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT p;
            auto dc = BeginPaint(h, &p);
            RECT r;
            GetClientRect(h, &r);
            FillRect(dc, &r, background);
            Gdiplus::Graphics g(dc);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(header.get(), Gdiplus::Rect(px(80), px(26), px(240), px(65)));
            EndPaint(h, &p);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(w), RGB(255, 255, 255));
            SetTextColor(reinterpret_cast<HDC>(w), RGB(35, 35, 35));
            return reinterpret_cast<LRESULT>(background);
        case WM_DRAWITEM:
        {
            auto d = reinterpret_cast<DRAWITEMSTRUCT *>(l);
            if (d->CtlID != FLASH)
                break;
            auto color = failed                          ? RGB(145, 22, 22)
                         : (d->itemState & ODS_SELECTED) ? RGB(48, 48, 48)
                                                         : RGB(22, 22, 22);
            FillRect(d->hDC, &d->rcItem, background);
            auto brush = CreateSolidBrush(color);
            auto pen = CreatePen(PS_SOLID, 1, RGB(8, 8, 8));
            auto oldBrush = SelectObject(d->hDC, brush);
            auto oldPen = SelectObject(d->hDC, pen);
            RoundRect(d->hDC, 0, 0, d->rcItem.right, d->rcItem.bottom, px(10), px(10));
            SelectObject(d->hDC, oldBrush);
            SelectObject(d->hDC, oldPen);
            DeleteObject(brush);
            DeleteObject(pen);
            RECT area = d->rcItem;
            InflateRect(&area, -px(18), -px(12));
            RECT measured = area;
            auto f = step == Step::Idle ? large : bold;
            SelectObject(d->hDC, f);
            DrawTextW(d->hDC, caption.c_str(), -1, &measured,
                      DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
            int height = measured.bottom - measured.top;
            area.top = (d->rcItem.bottom - height) / 2;
            area.bottom = area.top + height;
            draw(d->hDC, caption, area, f, RGB(255, 255, 255),
                 DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            if (busy || percent > 0)
            {
                RECT bar = d->rcItem;
                bar.top = bar.bottom - px(4);
                bar.right = bar.left + (bar.right - bar.left) * percent / 100;
                auto red = CreateSolidBrush(RGB(220, 15, 20));
                FillRect(d->hDC, &bar, red);
                DeleteObject(red);
            }
            if (d->itemState & ODS_FOCUS)
            {
                auto border = d->rcItem;
                InflateRect(&border, -4, -4);
                DrawFocusRect(d->hDC, &border);
            }
            return TRUE;
        }
        case EVENT:
        {
            std::unique_ptr<Update> u(reinterpret_cast<Update *>(l));
            caption = u->text;
            percent = u->progress;
            refresh();
            return 0;
        }
        case DONE:
        {
            if (worker.joinable())
                worker.join();
            busy = false;
            if (int(w) < 0)
            {
                failed = true;
                if (session && session->report.value("write_started", false))
                {
                    step = Step::Verify;
                }
                refresh();
            }
            else
            {
                step = Step(int(w));
                prompt();
            }
            return 0;
        }
        case WM_COMMAND:
            if (busy && LOWORD(w) != REPORTS)
                return 0;
            if (HIWORD(w) == CBN_SELCHANGE)
            {
                prompt();
                return 0;
            }
            switch (LOWORD(w))
            {
            case FLASH:
                flash();
                break;
            case BROWSE:
                browse();
                break;
            case REPORTS:
                ShellExecuteW(h, L"open", reportRoot().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            case BACK:
                step = Step::Idle;
                session.reset();
                prompt();
                break;
            }
            return 0;
        case WM_CLOSE:
            if (busy)
                return 0;
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            saveUI(false);
            PostQuitMessage(0);
            return 0;
        }
    }
    catch (const std::exception &e)
    {
        caption = wide(e.what());
        failed = true;
        refresh(false);
        return msg == WM_CREATE ? -1 : 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR args, int show)
{
    if (std::wstring(args) == L"--status")
    {
        try
        {
            Json j;
            auto r = reportRoot();
            if (fs::exists(r / L"ui.json"))
                j["ui"] = jsonRead(r / L"ui.json");
            j["latest"] = latestReport(r);
            auto s = j.dump(2);
            DWORD n;
            return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), DWORD(s.size()), &n,
                             nullptr)
                       ? 0
                       : 1;
        }
        catch (...)
        {
            return 1;
        }
    }
    if (*args)
    {
        MessageBoxW(nullptr, L"Unsupported command line option.", L"DRL Flasher",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\DRLFlasher.Native.1");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&ic);
    WNDCLASSW c{};
    c.lpfnWndProc = procedure;
    c.hInstance = instance;
    c.lpszClassName = L"DRLFlasher";
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.hbrBackground = background;
    RegisterClassW(&c);
    dpi = GetDpiForSystem();
    RECT r = {0, 0, px(400), px(500)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    win = CreateWindowExW(0, c.lpszClassName, L"DRL Flasher", style, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance, nullptr);
    COLORREF captionColor = RGB(255, 255, 255), textColor = RGB(20, 20, 20);
    DwmSetWindowAttribute(win, 35, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(win, 36, &textColor, sizeof(textColor));
    ShowWindow(win, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(win, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    if (worker.joinable())
        worker.join();
    header.reset();
    if (headerStream)
        headerStream->Release();
    Gdiplus::GdiplusShutdown(token);
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
}
