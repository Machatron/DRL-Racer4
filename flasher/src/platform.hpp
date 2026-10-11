#pragma once
#include "core.hpp"
#include <setupapi.h>
#include <devguid.h>
#include <shlobj.h>
#include <thread>
namespace drl
{
inline fs::path exeDir()
{
    wchar_t b[32768];
    GetModuleFileNameW(nullptr, b, 32768);
    return fs::path(b).parent_path();
}
inline fs::path reportRoot()
{
    PWSTR p = nullptr;
    require(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)),
            "LocalAppData unavailable.");
    fs::path r = fs::path(p) / L"DRL Flasher";
    CoTaskMemFree(p);
    fs::create_directories(r);
    return r;
}
inline Json latestReport(const fs::path &root)
{
    std::vector<fs::path> reports;
    for (auto &entry : fs::directory_iterator(root))
    {
        auto name = entry.path().filename().u8string();
        if (name.size() == 20 && name[8] == 'T' && name[15] == '_' && name[19] == 'Z' &&
            entry.is_directory() && fs::exists(entry.path() / L"report.json"))
            reports.push_back(entry.path() / L"report.json");
    }
    if (!reports.empty())
    {
        std::sort(reports.rbegin(), reports.rend());
        return jsonRead(reports.front());
    }
    return fs::exists(root / L"latest.json") ? jsonRead(root / L"latest.json") : Json();
}
inline std::vector<fs::path> bootDrives()
{
    std::vector<fs::path> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++)
        if (mask & (1 << i))
        {
            std::wstring s = L"A:\\";
            s[0] += i;
            fs::path p = s;
            std::error_code ec;
            if (fs::exists(p / L"INFO_UF2.TXT", ec) && fs::exists(p / L"CURRENT.UF2", ec))
            {
                auto b = read(p / L"INFO_UF2.TXT");
                std::string info(b.begin(), b.end());
                if (info.find("SAMD51J19A") != std::string::npos)
                    out.push_back(p);
            }
        }
    return out;
}
inline fs::path oneDrive()
{
    auto d = bootDrives();
    require(d.size() == 1,
            d.empty()
                ? "METROM4BOOT not found. Check the USB connection and bootloader mode."
                : "More than one boot drive is connected. Connect only the board being updated.");
    return d[0];
}
struct Port
{
    std::wstring name, label;
};
inline std::vector<Port> ports()
{
    std::vector<Port> out;
    HDEVINFO h = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE)
        return out;
    for (DWORD i = 0;; i++)
    {
        SP_DEVINFO_DATA d{};
        d.cbSize = sizeof(d);
        if (!SetupDiEnumDeviceInfo(h, i, &d))
            break;
        wchar_t instance[1024] = {};
        SetupDiGetDeviceInstanceIdW(h, &d, instance, 1024, nullptr);
        if (std::wstring(instance).find(L"USB\\") != 0)
            continue;
        wchar_t label[512] = {};
        SetupDiGetDeviceRegistryPropertyW(h, &d, SPDRP_FRIENDLYNAME, nullptr,
                                          reinterpret_cast<PBYTE>(label), sizeof(label), nullptr);
        HKEY k = SetupDiOpenDevRegKey(h, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE)
            continue;
        wchar_t port[256] = {};
        DWORD size = sizeof(port);
        auto err = RegQueryValueExW(k, L"PortName", nullptr, nullptr, reinterpret_cast<PBYTE>(port),
                                    &size);
        RegCloseKey(k);
        if (err == ERROR_SUCCESS && std::wstring(port).find(L"COM") == 0)
            out.push_back({port, label[0] ? label : port});
    }
    SetupDiDestroyDeviceInfoList(h);
    return out;
}
inline void enterBootloader(const std::wstring &port, bool fc)
{
    HANDLE h = CreateFileW((L"\\\\.\\" + port).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    require(h != INVALID_HANDLE_VALUE,
            "Port is busy or disconnected. Close Configurator and Serial Monitor.");
    try
    {
        DCB d{};
        d.DCBlength = sizeof(d);
        require(GetCommState(h, &d), "Cannot read serial settings.");
        d.BaudRate = fc ? 115200 : 1200;
        d.ByteSize = 8;
        d.Parity = NOPARITY;
        d.StopBits = ONESTOPBIT;
        d.fBinary = TRUE;
        d.fDtrControl = DTR_CONTROL_ENABLE;
        d.fRtsControl = RTS_CONTROL_DISABLE;
        d.fOutxCtsFlow = FALSE;
        d.fOutxDsrFlow = FALSE;
        d.fOutX = FALSE;
        d.fInX = FALSE;
        require(SetCommState(h, &d), "Cannot configure port.");
        COMMTIMEOUTS t = {20, 0, 120, 0, 500};
        SetCommTimeouts(h, &t);
        if (fc)
        {
            PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
            DWORD n;
            const char c[] = "#\r";
            require(WriteFile(h, c, 2, &n, nullptr) && n == 2, "CLI entry failed.");
            std::string reply;
            ULONGLONG until = GetTickCount64() + 2500;
            char b[1024];
            while (GetTickCount64() < until)
            {
                DWORD got = 0;
                ReadFile(h, b, sizeof(b), &got, nullptr);
                reply.append(b, got);
            }
            require(reply.find("CLI") != std::string::npos,
                    "No Betaflight CLI reply. Nothing sent to bootloader.");
            require(WriteFile(h, "bl\r", 3, &n, nullptr) && n == 3, "Bootloader request failed.");
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            EscapeCommFunction(h, CLRDTR);
        }
    }
    catch (...)
    {
        CloseHandle(h);
        throw;
    }
    CloseHandle(h);
}
inline fs::path cube()
{
    auto p = exeDir() / L"tools/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe";
    require(fs::is_regular_file(p),
            "Programming tool is missing. Extract the complete DRL Flasher ZIP and try again.");
    return p;
}
inline bool dfuConnected()
{
    HDEVINFO devices = SetupDiGetClassDevsW(nullptr, L"USB", nullptr,
                                           DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE)
        return false;
    bool found = false;
    for (DWORD i = 0;; i++)
    {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, i, &device))
            break;
        wchar_t id[1024] = {};
        if (SetupDiGetDeviceInstanceIdW(devices, &device, id, 1024, nullptr) &&
            _wcsnicmp(id, L"USB\\VID_0483&PID_DF11\\", 22) == 0)
            found = true;
    }
    SetupDiDestroyDeviceInfoList(devices);
    return found;
}
inline void installDfuDriver()
{
    auto inf = exeDir() / L"tools/STM32CubeProgrammer/Drivers/STM32Bootloader.inf";
    require(fs::is_regular_file(inf),
            "USB driver is missing. Extract the complete DRL Flasher ZIP and try again.");
    wchar_t system[MAX_PATH] = {};
    require(GetSystemDirectoryW(system, MAX_PATH) != 0, "Cannot find Windows system directory.");
    auto utility = fs::path(system) / L"pnputil.exe";
    auto args = L"/add-driver \"" + inf.wstring() + L"\" /install";
    SHELLEXECUTEINFOW request{};
    request.cbSize = sizeof(request);
    request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    request.lpVerb = L"runas";
    request.lpFile = utility.c_str();
    request.lpParameters = args.c_str();
    request.nShow = SW_HIDE;
    auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto started = ShellExecuteExW(&request);
    if (SUCCEEDED(com))
        CoUninitialize();
    require(started && request.hProcess,
            "USB driver installation was not approved. Click Flash to retry.");
    auto wait = WaitForSingleObject(request.hProcess, 60000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(request.hProcess, &code);
    CloseHandle(request.hProcess);
    require(wait == WAIT_OBJECT_0, "USB driver installation is still running. Wait, then retry.");
    require(code == ERROR_SUCCESS || code == ERROR_SUCCESS_REBOOT_REQUIRED,
            "USB driver installation failed. Reconnect Betaflight USB and retry.");
    require(code != ERROR_SUCCESS_REBOOT_REQUIRED,
            "Windows needs a restart to finish installing the USB driver.");
    std::this_thread::sleep_for(std::chrono::seconds(2));
}
inline std::wstring quote(const std::wstring &s)
{
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (auto c : s)
    {
        if (c == L'\\')
        {
            slashes++;
            continue;
        }
        if (c == L'"')
        {
            out.append(slashes * 2 + 1, L'\\');
            out += c;
        }
        else
        {
            out.append(slashes, L'\\');
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}
inline std::string process(const fs::path &exe, const std::vector<std::wstring> &args,
                           const fs::path &log)
{
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE f = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    require(f != INVALID_HANDLE_VALUE, "Cannot create tool log.");
    std::wstring cmd = quote(exe.wstring());
    for (auto &a : args)
        cmd += L" " + quote(a);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = f;
    si.hStdError = f;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, exe.parent_path().c_str(), &si, &pi);
    CloseHandle(f);
    require(ok, "Cannot start STM32CubeProgrammer.");
    auto wait = WaitForSingleObject(pi.hProcess, 300000);
    DWORD code = 1;
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    else
        GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    require(wait == WAIT_OBJECT_0,
            "Programmer timed out. Leave the board in bootloader and keep the backup.");
    auto b = read(log);
    std::string result(b.begin(), b.end());
    require(code == 0, "Programmer failed. See " + log.u8string());
    return result;
}
struct Session
{
    fs::path root, folder, package, drive;
    Json catalog, profile, known, report;
    Bytes before, expected, payload;
    bool defaults = false, fc = false;
    std::string serial;
    std::function<void(std::string, int)> update;
    void state(const std::string &s, const std::string &message, int percent)
    {
        report["state"] = s;
        report["message"] = message;
        report["progress"] = percent;
        report["updated_utc"] = stamp();
        jsonWrite(folder / L"report.json", report);
        // Recovery reads the session report if the summary file is locked.
        tryJsonWrite(root / L"latest.json", report);
        std::ofstream log(folder / L"events.log", std::ios::app);
        log << stamp() << " " << s << " " << message << "\n";
        if (update)
            update(message, percent);
    }
    void start(const fs::path &pkg, const Json &cat, const std::string &key, bool reset)
    {
        root = reportRoot();
        folder = root / fs::u8path(stamp());
        fs::create_directory(folder);
        package = pkg;
        catalog = cat;
        profile = key == "LED_AUTO" ? Json{{"kind", "led_auto"}}
                : key == "FC_AUTO" ? Json{{"kind", "fc_auto"}}
                : key == "EC_AUTO" ? Json{{"kind", "ec_auto"}}
                                   : cat.at("profiles").at(key);
        defaults = reset;
        fc = profile.at("kind") == "fc" || profile.at("kind") == "fc_auto";
        if (fc)
            cube();
        report = {{"schema", 1},
                  {"session", folder.u8string()},
                  {"build", cat.at("name")},
                  {"profile", key},
                  {"settings", reset ? "build_defaults" : "keep_current"},
                  {"write_started", false},
                  {"verified", false}};
        report["requested_profile"] = key;
        jsonWrite(folder / L"catalog.json", cat);
        state("checking", "Checking build files", 2);
        if (key != "LED_AUTO" && key != "FC_AUTO" && key != "EC_AUTO")
            payload = verifiedFile(pkg, profile.at("image"));
    }
    std::string run(const std::string &label, const std::vector<std::wstring> &tail)
    {
        std::vector<std::wstring> args = {L"-c", L"port=USB1"};
        if (!serial.empty())
            args.push_back(L"sn=" + wide(serial));
        args.insert(args.end(), tail.begin(), tail.end());
        auto log = folder / fs::u8path(label + ".log");
        unsigned attempt = 1;
        while (fs::exists(log))
            log = folder / fs::u8path(label + "_" + std::to_string(++attempt) + ".log");
        return process(cube(), args, log);
    }
    Bytes ufRead(const std::string &name)
    {
        auto a = read(drive / L"CURRENT.UF2");
        auto b = read(drive / L"CURRENT.UF2");
        require(a == b, "Two flash reads differ. Check the cable and retry.");
        write(folder / fs::u8path(name + ".uf2"), a);
        return decodeUF2(a);
    }
    void prepare()
    {
        state("backing_up", "Reading the board twice", 10);
        if (fc)
        {
            auto list = process(cube(), {L"-l", L"usb"}, folder / L"devices.log");
            if (list.find("Serial number") == std::string::npos && dfuConnected())
            {
                state("usb_driver", "Installing USB driver. Accept the Windows prompt.", 5);
                installDfuDriver();
                list = process(cube(), {L"-l", L"usb"}, folder / L"devices_after_driver.log");
                state("backing_up", "Reading the board twice", 10);
            }
            auto pos = list.find("Serial number");
            require(pos != std::string::npos &&
                        list.find("Serial number", pos + 1) == std::string::npos,
                    "Connect exactly one STM32 DFU device. Use Enter bootloader, or the FC BOOT "
                    "button.");
            pos = list.find(':', pos);
            require(pos != std::string::npos, "Cannot read DFU serial number.");
            auto end = list.find_first_of("\r\n", pos);
            serial = list.substr(pos + 1, end - pos - 1);
            serial.erase(0, serial.find_first_not_of(" \t"));
            serial.erase(serial.find_last_not_of(" \t") + 1);
            require(!serial.empty(), "DFU serial is missing.");
            report["usb_serial"] = serial;
            auto detected = detectFC(list, catalog);
            if (profile.at("kind") == "fc_auto")
            {
                profile = catalog.at("profiles").at(detected);
                payload = verifiedFile(package, profile.at("image"));
                report["profile"] = detected;
            }
            else
                require(report.at("profile") == detected,
                        "Connected FC does not match the selected F4/F7 hardware.");
            report["detected_hardware"] = detected.substr(3);
            state("backing_up", "Reading the board twice", 10);
            size_t size = profile.at("flash_size");
            std::wstringstream ss;
            ss << L"0x" << std::hex << size;
            auto info = run("backup_1",
                            {L"-u", L"0x08000000", ss.str(), (folder / L"before.bin").wstring()});
            validateFC(info, profile, fs::file_size(folder / L"before.bin"));
            report["device_id"] = profile.at("device_id");
            run("backup_2", {L"-u", L"0x08000000", ss.str(), (folder / L"second.bin").wstring()});
            before = read(folder / L"before.bin");
            require(before.size() == size && before == read(folder / L"second.bin"),
                    "FC backup reads differ.");
        }
        else
        {
            drive = oneDrive();
            before = ufRead("before");
            write(folder / L"before.bin", before);
        }
        report["before_sha256"] = hash(before);
        if (profile.at("kind") == "ec_auto")
        {
            auto key = detectEC(before, catalog, package);
            profile = catalog.at("profiles").at(key);
            payload = verifiedFile(package, profile.at("image"));
            report["profile"] = key;
        }
        if (profile.at("kind") == "led_auto")
        {
            auto key = detectLED(before, catalog);
            profile = catalog.at("profiles").at(key);
            payload = verifiedFile(package, profile.at("image"));
            report["profile"] = key;
            report["detected_wiring"] = key.substr(4);
        }
        if (fc)
        {
            require(defaults, "FC flashing requires build defaults.");
            expected = fcExpected(before, payload, defaults);
            write(folder / L"firmware.hex", payload);
        }
        else
        {
            known = identify(before, profile);
            report["detected_firmware"] = known.at("name");
            expected = mergeSAMD(before, payload, known, false);
            write(folder / L"update.uf2", encodeUF2(expected));
            write(folder / L"rollback.uf2", encodeUF2(before));
        }
        write(folder / L"expected.bin", expected);
        report["expected_sha256"] = hash(expected);
        report["backup_file"] = (folder / L"before.bin").u8string();
        state("ready_to_flash", "Backup saved. Ready to flash.", 25);
    }
    void flash()
    {
        require(report.at("state") == "ready_to_flash", "A fresh backup is required.");
        // Read again before writing in case the connected board changed after planning.
        if (fc)
        {
            std::wstringstream ss;
            ss << L"0x" << std::hex << before.size();
            run("prewrite", {L"-u", L"0x08000000", ss.str(), (folder / L"prewrite.bin").wstring()});
            require(read(folder / L"prewrite.bin") == before,
                    "Board changed since backup. Check it again.");
        }
        else
        {
            require(oneDrive() == drive, "Boot drive changed. Check it again.");
            require(ufRead("prewrite") == before, "Board changed since backup. Check it again.");
        }
        report["write_started"] = true;
        state("writing", "Writing firmware. Keep power connected.", 40);
        if (fc)
        {
            run("program", {L"-w", (folder / L"firmware.hex").wstring(), L"-v"});
            if (defaults)
            {
                Bytes blank(0x4000, 255);
                write(folder / L"settings_reset.bin", blank);
                run("settings",
                    {L"-w", (folder / L"settings_reset.bin").wstring(), L"0x08004000", L"-v"});
            }
            verify();
        }
        else
        {
            require(
                CopyFileW((folder / L"update.uf2").c_str(), (drive / L"UPDATE.UF2").c_str(), FALSE),
                "UF2 copy failed. Keep the backup and reopen the bootloader.");
            state(
                "awaiting_readback",
                profile.at("kind") == "led"
                    ? "Disconnect battery power for 5 seconds and reconnect, then click to verify."
                    : "Double-press the EC RESET, then click to verify.",
                70);
        }
    }
    void verify()
    {
        if (!fc && profile.at("kind") == "led" && bootDrives().empty())
        {
            auto devices = ports();
            require(devices.size() == 1, "Connect only the LEDC USB JST with battery power "
                                         "connected, then retry verification.");
            state("entering_readback", "Entering bootloader...", 75);
            enterBootloader(devices[0].name, false);
            for (int i = 0; i < 40 && bootDrives().empty(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            require(!bootDrives().empty(), "LEDC bootloader did not appear. Disconnect battery "
                                           "power for 5 seconds and reconnect, then retry.");
        }
        state("verifying", "Reading back the complete flash", 85);
        Bytes after;
        if (fc)
        {
            std::wstringstream ss;
            ss << L"0x" << std::hex << before.size();
            run("readback", {L"-u", L"0x08000000", ss.str(), (folder / L"after.bin").wstring()});
            after = read(folder / L"after.bin");
        }
        else
        {
            drive = oneDrive();
            after = ufRead("after");
            write(folder / L"after.bin", after);
        }
        report["readback_sha256"] = hash(after);
        require(after == expected, "Readback differs from the prepared image. Keep the board in "
                                   "bootloader; the backup is in Reports.");
        report["verified"] = true;
        if (fc)
        {
            state("verified", "Flash verified. Starting the FC.", 100);
            run("boot", {L"-g", L"0x08000000"});
        }
        state("complete",
              fc ? "Verified. Reconnect Betaflight USB, then check settings."
              : profile.at("kind") == "led" ? "Verified. Disconnect battery power for 5 seconds "
                                              "and reconnect to start the LEDC."
                                            : "Verified. Press EC RESET once to start the board.",
              100);
    }
    void fail(const std::string &message)
    {
        state(report.value("write_started", false) ? "needs_attention" : "stopped", message,
              report.value("progress", 0));
    }
};
} // namespace drl
