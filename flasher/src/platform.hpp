#pragma once
#include "core.hpp"
#include "osd.hpp"
#include "samba.hpp"
#include "font_applet.hpp"
#include "prompts.hpp"
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
    bool restoring = false;
    Backup backup;
    std::string serial;
    std::function<void(std::string, int)> update;
    fs::path file(const std::string &name) const
    {
        return reportFile(folder, report, name);
    }
    void state(const std::string &s, const std::string &message, int percent)
    {
        report["state"] = s;
        report["message"] = message;
        report["progress"] = percent;
        report["progress_scope"] = "step";
        report["updated_utc"] = stamp();
        jsonWrite(folder / L"report.json", report);
        std::ofstream log(folder / L"events.log", std::ios::app);
        log << stamp() << " " << s << " " << message << "\n";
        if (update)
            update(message, percent);
    }
    void start(const AppSession &app, const fs::path &pkg, const Json &cat,
               const std::string &key, bool reset)
    {
        root = app.root;
        folder = app.nextOperation(key);
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
        report = {{"schema", 3},
                  {"session", app.folder.u8string()},
                  {"operation_folder", folder.u8string()},
                  {"build", cat.at("name")},
                  {"profile", key},
                  {"settings", reset ? "build_defaults" : "keep_current"},
                  {"write_started", false},
                  {"verified", false}};
        report["requested_profile"] = key;
        report["package"] = pkg.u8string();
        jsonWrite(folder / L"catalog.json", cat);
        state("checking", "Checking build files", -1);
        if (key.rfind("EC_", 0) == 0)
        {
            for (auto layout : {"original", "betaflight"})
            {
                auto font = parseFont(verifiedFile(pkg, cat.at("fonts").at(layout)));
                write(file(std::string("font_") + layout + ".bin"), font);
                report["font_assets"][layout] = hash(font);
            }
        }
        if (key != "LED_AUTO" && key != "FC_AUTO" && key != "EC_AUTO")
            payload = verifiedFile(pkg, profile.at("image"));
    }
    void restore(const AppSession &app, const fs::path &pkg, const Json &cat, const Backup &saved)
    {
        start(app, pkg, cat, saved.key, false);
        backup = saved;
        restoring = true;
        report["operation"] = "restore";
        report["restore_source"] = saved.file.u8string();
        report["restore_sha256"] = hash(saved.image);
        report["settings"] = "backup";
        state("restore_connect", stepText(Step::Connect, profile.at("kind"), true), 0);
    }
    void queueFont(const std::string &layout)
    {
        require(profile.at("kind") == "ec", "Font programming belongs to the EC operation.");
        auto font = restoring && layout == "backup" ? backup.fontData
                                                    : read(file("font_" + layout + ".bin"));
        require(hash(font) == (layout == "backup" ? Json(backup.fontHash)
                                                  : report.at("font_assets").at(layout)),
                "Bundled font changed.");
        write(file("font_expected.bin"), font);
        report["font_layout"] = layout;
        report["font_expected_sha256"] = hash(font);
        report["font_verified"] = false;
        report["font_before_restore"] = false;
        report["font_pending"] = true;
        state("awaiting_font", stepText(Step::Font, profile.at("kind"), restoring), 0);
    }
    void uploadFont()
    {
        require(profile.at("kind") == "ec", "Font programming belongs to the EC operation.");
        auto expectedFont = read(file("font_expected.bin"));
        require(hash(expectedFont) == report.at("font_expected_sha256"), "Font file changed.");
        require(report.value("verified", false), "Verify the EC firmware before the font step.");
        state("font_connecting", "Connecting to the OSD", -1);
        require(!bootDrives().empty(), "Double press EC RESET, then retry the font step.");
        drive = oneDrive();
        require(ufRead("font_firmware") == expected,
                "EC firmware changed since verification. Check the connected board.");
        auto devices = ports();
        require(devices.size() == 1, "Connect only EC USB with battery power connected.");
        report["font_transport"] = "ec_bootloader_ram";
        report["font_bootloader"] = fontBootloader(expected);
        report["font_helper_sha256"] = hash(Bytes(FontApplet, FontApplet + sizeof(FontApplet)));
        BootFont connection(devices.front().name, expected,
                            Bytes(FontApplet, FontApplet + sizeof(FontApplet)));
        connection.request(2);
        auto readGlyph = [&](uint8_t index) { return connection.readGlyph(index); };
        auto readFont = [&](const std::string &phase, const std::string &label)
        {
            state(phase, label, 0);
            Bytes result;
            for (unsigned index = 0; index < 256; ++index)
            {
                auto glyph = readGlyph(index);
                result.insert(result.end(), glyph.begin(), glyph.end());
                if ((index + 1) % 16 == 0)
                    state(phase, label + " " + std::to_string(index + 1) + " / 256", (index + 1) * 100 / 256);
            }
            return result;
        };
        auto installed = readFont("font_check", "Reading OSD font");
        report["font_unchanged"] = installed == expectedFont;
        report["font_write_attempts"] = 0;
        if (installed != expectedFont && !fs::exists(file("font_before.bin")))
        {
            require(installed == readFont("font_backup", "Checking font backup"),
                    "The two font backups differ. Nothing written to the OSD.");
            write(file("font_before.bin"), installed);
            report["font_before_sha256"] = hash(installed);
            exportBackups(folder, report);
            state("font_backup_saved", "OSD font backup saved", 100);
        }
        else if (fs::exists(file("font_before.bin")))
            require(hash(read(file("font_before.bin"))) == report.at("font_before_sha256"),
                    "The saved font backup changed.");
        auto writeGlyph = [&](uint8_t index, const Bytes &glyph)
        {
            report["font_write_attempts"] = report.at("font_write_attempts").get<unsigned>() + 1;
            connection.writeGlyph(index, glyph);
        };
        auto after = installed;
        if (installed == expectedFont)
            state("font_unchanged", "OSD font already correct", 100);
        else
        {
            state("font_writing", "Writing OSD font", 0);
            after = programFont(expectedFont, readGlyph, writeGlyph, [&](unsigned count)
            {
                if (count % 16 == 0)
                    state("font_writing", "Writing OSD font " + std::to_string(count) + " / 256", count * 100 / 256);
                if (count == 256)
                    state("font_verifying", "Verifying OSD font", 0);
            }, installed, [&](unsigned count)
            {
                if (count % 16 == 0)
                    state("font_verifying", "Verifying OSD font " + std::to_string(count) + " / 256", count * 100 / 256);
            });
        }
        write(file("font_after.bin"), after);
        report["font_readback_sha256"] = hash(after);
        state("font_final_check", "Checking EC flash", -1);
        require(ufRead("font_final_firmware") == expected,
                "EC flash differs after the font operation. Keep the backup in Reports.");
        report["font_verified"] = true;
        report["font_pending"] = false;
        exportBackups(folder, report);
        state("complete", stepText(Step::Complete, profile.at("kind"), restoring), 100);
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
        write(file(name + ".uf2"), a);
        return decodeUF2(a);
    }
    void prepare()
    {
        state("backing_up", "Reading the board twice", -1);
        if (fc)
        {
            auto list = process(cube(), {L"-l", L"usb"}, folder / L"devices.log");
            if (list.find("Serial number") == std::string::npos && dfuConnected())
            {
                state("usb_driver", "Installing USB driver. Accept the Windows prompt.", -1);
                installDfuDriver();
                list = process(cube(), {L"-l", L"usb"}, folder / L"devices_after_driver.log");
                state("backing_up", "Reading the board twice", -1);
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
            state("backing_up", "Reading the board twice", -1);
            size_t size = profile.at("flash_size");
            std::wstringstream ss;
            ss << L"0x" << std::hex << size;
            auto info = run("backup_1",
                            {L"-u", L"0x08000000", ss.str(), file("before.bin").wstring()});
            validateFC(info, profile, fs::file_size(file("before.bin")));
            report["device_id"] = profile.at("device_id");
            run("backup_2", {L"-u", L"0x08000000", ss.str(), file("second.bin").wstring()});
            before = read(file("before.bin"));
            require(before.size() == size && before == read(file("second.bin")),
                    "FC backup reads differ.");
        }
        else
        {
            drive = oneDrive();
            before = ufRead("before");
            write(file("before.bin"), before);
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
        if (profile.at("kind") == "ec")
            report["font_bootloader"] = fontBootloader(before);
        if (restoring)
        {
            expected = restoreImage(before, backup, catalog, report.at("profile"));
            if (fc)
                write(file("restore.bin"), expected);
            else
                write(file("update.uf2"), encodeUF2(expected));
        }
        else if (fc)
        {
            require(defaults, "FC flashing requires build defaults.");
            expected = fcExpected(before, payload, defaults);
            write(file("firmware.hex"), payload);
        }
        else
        {
            known = identify(before, profile);
            report["detected_firmware"] = known.at("name");
            expected = mergeSAMD(before, payload, known, false);
            write(file("update.uf2"), encodeUF2(expected));
            write(file("rollback.uf2"), encodeUF2(before));
        }
        write(file("expected.bin"), expected);
        report["expected_sha256"] = hash(expected);
        report["backup_file"] = file("before.bin").u8string();
        exportBackups(folder, report);
        state("ready_to_flash", "Backup saved. Ready to flash.", -1);
    }
    void flash()
    {
        require(report.at("state") == "ready_to_flash", "A fresh backup is required.");
        // Read again before writing in case the connected board changed after planning.
        if (fc)
        {
            std::wstringstream ss;
            ss << L"0x" << std::hex << before.size();
            run("prewrite", {L"-u", L"0x08000000", ss.str(), file("prewrite.bin").wstring()});
            require(read(file("prewrite.bin")) == before,
                    "Board changed since backup. Check it again.");
        }
        else
        {
            require(oneDrive() == drive, "Boot drive changed. Check it again.");
            require(ufRead("prewrite") == before, "Board changed since backup. Check it again.");
        }
        report["write_started"] = true;
        state("writing", "Writing firmware", -1);
        if (fc)
        {
            if (restoring)
                run("restore", {L"-w", file("restore.bin").wstring(), L"0x08000000", L"-v"});
            else
                run("program", {L"-w", file("firmware.hex").wstring(), L"-v"});
            if (defaults)
            {
                Bytes blank(0x4000, 255);
                write(file("settings_reset.bin"), blank);
                run("settings",
                    {L"-w", file("settings_reset.bin").wstring(), L"0x08004000", L"-v"});
            }
            verify();
        }
        else
        {
            require(
                CopyFileW(file("update.uf2").c_str(), (drive / L"UPDATE.UF2").c_str(), FALSE),
                "UF2 copy failed. Keep the backup and reopen the bootloader.");
            state("awaiting_readback", stepText(Step::Verify, profile.at("kind"), restoring), 0);
        }
    }
    void verify()
    {
        if (!fc && profile.at("kind") == "led" && bootDrives().empty())
        {
            auto devices = ports();
            require(devices.size() == 1, "Connect only the LEDC USB JST with battery power "
                                         "connected, then retry verification.");
            state("entering_readback", "Entering bootloader...", -1);
            enterBootloader(devices[0].name, false);
            for (int i = 0; i < 40 && bootDrives().empty(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            require(!bootDrives().empty(), "LEDC bootloader did not appear. Disconnect battery "
                                           "power for 5 seconds and reconnect, then retry.");
        }
        state("verifying", "Reading back the complete flash", -1);
        Bytes after;
        if (fc)
        {
            std::wstringstream ss;
            ss << L"0x" << std::hex << before.size();
            run("readback", {L"-u", L"0x08000000", ss.str(), file("after.bin").wstring()});
            after = read(file("after.bin"));
        }
        else
        {
            drive = oneDrive();
            after = ufRead("after");
            write(file("after.bin"), after);
        }
        report["readback_sha256"] = hash(after);
        require(after == expected, "Readback differs from the prepared image. Keep the board in "
                                   "bootloader; the backup is in Reports.");
        report["verified"] = true;
        if (fc)
        {
            state("verified", "Flash verified. Starting the FC.", -1);
            run("boot", {L"-g", L"0x08000000"});
        }
        if (profile.at("kind") == "ec")
        {
            queueFont(restoring ? backup.font : "betaflight");
            return;
        }
        state("complete", stepText(Step::Complete, profile.at("kind"), restoring), 100);
    }
    void fail(const std::string &message)
    {
        state(report.value("write_started", false) ? "needs_attention" : "stopped", message,
              report.value("progress", 0));
    }
};
} // namespace drl
