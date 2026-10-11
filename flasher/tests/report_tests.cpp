#include "../src/platform.hpp"
#include <cassert>
#include <iostream>

using namespace drl;

struct ReadLock
{
    HANDLE handle;
    explicit ReadLock(const fs::path &path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        assert(handle != INVALID_HANDLE_VALUE);
    }
    void close()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    ~ReadLock()
    {
        close();
    }
};

int main(int argc, char **argv)
{
    auto root =
        fs::temp_directory_path() / (L"DRL-report-tests-" + std::to_wstring(GetCurrentProcessId()) +
                                     L"-" + std::to_wstring(GetTickCount64()));
    try
    {
        assert(argc == 2);
        auto package = fs::u8path(argv[1]);
        auto catalog = jsonRead(package / L"bundle.json");
        fs::create_directory(root);
        auto path = root / L"report.json";
        jsonWrite(path, {{"value", 1}});
        {
            ReadLock lock(path);
            bool rejected = false;
            try
            {
                jsonWrite(path, {{"value", 2}}, 2);
            }
            catch (const std::exception &error)
            {
                auto message = std::string(error.what());
                rejected = message.find("report.json") != std::string::npos &&
                           message.find("Windows error") != std::string::npos;
            }
            assert(rejected);
            assert(!tryJsonWrite(path, {{"value", 2}}));
            assert(jsonRead(path).at("value") == 1);
            for (auto &entry : fs::directory_iterator(root))
                assert(entry.path().extension() != L".tmp");
        }
        {
            ReadLock lock(path);
            std::atomic<bool> saved{false};
            std::thread writer([&] {
                try
                {
                    jsonWrite(path, {{"value", 3}});
                    saved = true;
                }
                catch (...)
                {
                }
            });
            Sleep(100);
            lock.close();
            writer.join();
            assert(saved && jsonRead(path).at("value") == 3);
        }
        std::atomic<unsigned> failures{0};
        std::vector<std::thread> writers;
        for (unsigned writer = 0; writer < 3; ++writer)
            writers.emplace_back([&, writer] {
                try
                {
                    for (unsigned i = 0; i < 30; ++i)
                        jsonWrite(path,
                                  {{"value", writer}, {"text", std::string(6000, 'a' + writer)}});
                }
                catch (...)
                {
                    ++failures;
                }
            });
        for (unsigned i = 0; i < 150; ++i)
        {
            try
            {
                assert(jsonRead(path).contains("value"));
            }
            catch (...)
            {
                ++failures;
            }
        }
        for (auto &writer : writers)
            writer.join();
        assert(failures == 0);

        Session session;
        session.root = root;
        session.folder = root / L"20261011T130552_351Z";
        fs::create_directory(session.folder);
        session.report = {{"profile", "LED_AB"},
                          {"write_started", true},
                          {"verified", false},
                          {"session", session.folder.u8string()}};
        jsonWrite(root / L"latest.json", {{"state", "old_session"}});
        bool notified = false;
        session.update = [&](std::string, int progress) { notified = progress == 85; };
        {
            ReadLock lock(root / L"latest.json");
            session.state("verifying", "Reading back the complete flash", 85);
            assert(notified);
            auto report = latestReport(root);
            assert(report.at("state") == "verifying" && report.at("write_started") == true);
        }
        {
            ReadLock lock(session.folder / L"report.json");
            bool rejected = false;
            try
            {
                session.state("complete", "Verified", 100);
            }
            catch (...)
            {
                rejected = true;
            }
            assert(rejected);
            assert(latestReport(root).at("state") == "verifying");
        }
        session.report["verified"] = true;
        session.state("complete", "Verified", 100);
        assert(latestReport(root).at("verified") == true);

        AppSession firstLaunch;
        firstLaunch.start(root);
        Session fc, ec, led, anotherLed;
        fc.start(firstLaunch, package, catalog, "FC_AUTO", true);
        ec.start(firstLaunch, package, catalog, "EC_AUTO", false);
        led.start(firstLaunch, package, catalog, "LED_AUTO", false);
        anotherLed.start(firstLaunch, package, catalog, "LED_AUTO", false);
        fc.profile = catalog.at("profiles").at("FC_F7");
        ec.profile = catalog.at("profiles").at("EC_F7");
        assert(!fc.report.contains("font_assets") && !led.report.contains("font_assets") &&
               !fs::exists(fc.file("font_original.bin")) &&
               !fs::exists(fc.file("font_betaflight.bin")));
        bool fcFontRejected = false;
        try
        {
            fc.queueFont("original");
        }
        catch (...)
        {
            fcFontRejected = true;
        }
        assert(fcFontRejected && !fc.report.value("font_pending", false));
        for (auto font : {"original", "betaflight"})
        {
            ec.queueFont(font);
            assert(ec.report.at("font_pending") == true &&
                   ec.report.at("font_before_restore") == false);
            assert(read(ec.file("font_expected.bin")) ==
                   read(ec.file(std::string("font_") + font + ".bin")));
        }
        assert(fc.folder.filename() == L"01 FC" && ec.folder.filename() == L"02 EC" &&
               led.folder.filename() == L"03 LEDC" && anotherLed.folder.filename() == L"04 LEDC");
        for (auto *operation : {&fc, &ec, &led, &anotherLed})
        {
            assert(operation->folder.parent_path() == firstLaunch.data());
            auto backupPath = operation->file("before.bin");
            auto expectedPath = operation->file("expected.bin");
            write(backupPath, Bytes{1, 2, 3});
            write(expectedPath, Bytes{4, 5, 6});
            operation->report["before_sha256"] = hash(read(backupPath));
            operation->report["expected_sha256"] = hash(read(expectedPath));
            operation->report["backup_file"] = backupPath.u8string();
            operation->report["write_started"] = true;
            exportBackups(operation->folder, operation->report);
            operation->state("awaiting_readback", "Readback pending", 70);
        }
        auto autoName = fc.file("before.bin");
        fc.report["profile"] = "FC_F7";
        assert(fc.file("before.bin") == autoName);
        led.report["profile"] = "LED_C";
        anotherLed.report["profile"] = "LED_AB";
        anotherLed.state("awaiting_readback", "Readback pending", 70);
        assert(latestReport(root).at("operation_folder") == anotherLed.folder.u8string());

        AppSession secondLaunch;
        secondLaunch.start(root);
        assert(firstLaunch.folder != secondLaunch.folder);
        assert(latestReport(root).at("operation_folder") == anotherLed.folder.u8string());
        auto previous = read(anotherLed.folder / L"report.json");
        auto resumed = secondLaunch.resume(latestReport(root));
        auto resumedFolder = reportFolder(root, resumed);
        assert(resumedFolder.parent_path() == secondLaunch.data() &&
               resumedFolder.filename() == L"01 LEDC");
        assert(read(reportFile(resumedFolder, resumed, "before.bin")) == Bytes({1, 2, 3}) &&
               read(reportFile(resumedFolder, resumed, "expected.bin")) == Bytes({4, 5, 6}) &&
               read(anotherLed.folder / L"report.json") == previous);
        assert(resumed.at("backup_file") ==
                   (secondLaunch.folder / "01 LEDC backup.bin").u8string() &&
               read(secondLaunch.folder / "01 LEDC backup.bin") == Bytes({1, 2, 3}));
        write(session.folder / L"before.bin", Bytes{7, 8});
        auto legacy = secondLaunch.resume(session.report);
        assert(read(reportFile(reportFolder(root, legacy), legacy, "before.bin")) == Bytes({7, 8}));
        bool outsideRejected = false;
        try
        {
            auto outside = resumed;
            outside["operation_folder"] = fs::temp_directory_path().u8string();
            reportFolder(root, outside);
        }
        catch (...)
        {
            outsideRejected = true;
        }
        assert(outsideRejected);
        jsonWrite(firstLaunch.data() / L"ui.json",
                  {{"updated_utc", "20261011T000000_000Z"}, {"pid", 1}});
        jsonWrite(secondLaunch.data() / L"ui.json",
                  {{"updated_utc", "20261011T000001_000Z"}, {"pid", 2}});
        assert(latestRecord(root, true).at("pid") == 2);
        assert(fs::canonical(root).parent_path() == fs::canonical(fs::temp_directory_path()) &&
               root.filename().u8string().rfind("DRL-report-tests-", 0) == 0);
        fs::remove_all(root);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << "\nTest files: " << root << '\n';
        return 1;
    }
}
