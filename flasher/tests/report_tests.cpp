#include "../src/platform.hpp"
#include <iostream>

using namespace drl;

struct ReadLock
{
    HANDLE handle;
    explicit ReadLock(const fs::path &path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        require(handle != INVALID_HANDLE_VALUE, "Cannot lock test report.");
    }
    void close()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    ~ReadLock() { close(); }
};

int main()
{
    auto root = fs::temp_directory_path() /
                (L"DRL-report-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()));
    try
    {
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
            require(rejected, "A locked report must return a useful error.");
            require(!tryJsonWrite(path, {{"value", 2}}), "Status writes must not throw.");
            require(jsonRead(path).at("value") == 1, "Failed writes must preserve the report.");
            for (auto &entry : fs::directory_iterator(root))
                require(entry.path().extension() != L".tmp", "Failed write left a temporary file.");
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
                catch (...) {}
            });
            Sleep(100);
            lock.close();
            writer.join();
            require(saved && jsonRead(path).at("value") == 3,
                    "A temporary read lock must be retried.");
        }
        std::atomic<unsigned> failures{0};
        std::vector<std::thread> writers;
        for (unsigned writer = 0; writer < 3; ++writer)
            writers.emplace_back([&, writer] {
                try
                {
                    for (unsigned i = 0; i < 30; ++i)
                        jsonWrite(path, {{"value", writer}, {"text", std::string(6000, 'a' + writer)}});
                }
                catch (...) { ++failures; }
            });
        for (unsigned i = 0; i < 150; ++i)
        {
            try { require(jsonRead(path).contains("value"), "Partial report."); }
            catch (...) { ++failures; }
        }
        for (auto &writer : writers)
            writer.join();
        require(failures == 0, "Concurrent report readers and writers failed.");

        Session session;
        session.root = root;
        session.folder = root / L"20261011T130552_351Z";
        fs::create_directory(session.folder);
        session.report = {{"write_started", true}, {"verified", false},
                          {"session", session.folder.u8string()}};
        jsonWrite(root / L"latest.json", {{"state", "old_session"}});
        bool notified = false;
        session.update = [&](std::string, int progress) { notified = progress == 85; };
        {
            ReadLock lock(root / L"latest.json");
            session.state("verifying", "Reading back the complete flash", 85);
            require(notified, "A locked summary must not interrupt verification.");
            auto report = latestReport(root);
            require(report.at("state") == "verifying" && report.at("write_started") == true,
                    "Recovery must find the session when the summary is stale.");
        }
        {
            ReadLock lock(session.folder / L"report.json");
            bool rejected = false;
            try { session.state("complete", "Verified", 100); }
            catch (...) { rejected = true; }
            require(rejected, "Session report failure must remain visible.");
            require(latestReport(root).at("state") == "verifying",
                    "A failed session write must preserve the recovery record.");
        }
        session.report["verified"] = true;
        session.state("complete", "Verified", 100);
        require(latestReport(root).at("verified") == true, "Completed report was not saved.");
        fs::remove_all(root);
        std::cout << "Report locking, concurrent access and recovery checks passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << "\nTest files: " << root << '\n';
        return 1;
    }
}
