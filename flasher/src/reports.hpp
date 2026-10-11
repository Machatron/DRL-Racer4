#pragma once
#include "core.hpp"

namespace drl
{
inline std::string boardLabel(const std::string &key)
{
    if (key.rfind("FC_", 0) == 0)
        return "FC";
    if (key.rfind("EC_", 0) == 0)
        return "EC";
    require(key.rfind("LED_", 0) == 0, "Unknown board type.");
    return "LEDC";
}

inline fs::path reportFile(const fs::path &folder, const Json &report, const std::string &name)
{
    if (report.value("schema", 1) < 2)
        return safeFile(folder, name);
    static const std::map<std::string, std::string> names = {
        {"before.bin", "backup.bin"},
        {"before.uf2", "backup.uf2"},
        {"second.bin", "backup check.bin"},
        {"after.bin", "readback.bin"},
        {"after.uf2", "readback.uf2"},
        {"prewrite.bin", "before writing.bin"},
        {"prewrite.uf2", "before writing.uf2"},
        {"expected.bin", "expected.bin"},
        {"restore.bin", "restore.bin"},
        {"rollback.uf2", "restore.uf2"},
        {"update.uf2", "update.uf2"},
        {"firmware.hex", "update.hex"},
        {"settings_reset.bin", "settings reset.bin"}};
    static const std::map<std::string, std::string> fonts = {
        {"font_before.bin", "OSD font backup.bin"},
        {"font_after.bin", "OSD font readback.bin"},
        {"font_expected.bin", "OSD font expected.bin"},
        {"font_original.bin", "OSD font original.bin"},
        {"font_betaflight.bin", "OSD font Betaflight.bin"},
        {"font_firmware.uf2", "EC before font.uf2"},
        {"font_final_firmware.uf2", "EC after font.uf2"}};
    auto font = fonts.find(name);
    if (font != fonts.end())
        return folder / fs::u8path(font->second);
    auto item = names.find(name);
    require(item != names.end(), "Unknown backup file.");
    return folder / fs::u8path(boardLabel(report.at("profile")) + " " + item->second);
}

inline fs::path reportFolder(const fs::path &root, const Json &report)
{
    auto folder = fs::weakly_canonical(
        fs::u8path(report.value("operation_folder", report.at("session").get<std::string>())));
    auto parent = folder.parent_path(), base = fs::weakly_canonical(root);
    require(parent == base || parent.parent_path() == base ||
                (parent.filename() == L"Data" && parent.parent_path().parent_path() == base),
            "Invalid report folder.");
    return folder;
}

inline void exportBackups(const fs::path &operation, Json &report)
{
    if (operation.parent_path().filename() != L"Data")
        return;
    auto root = operation.parent_path().parent_path();
    auto save = [&](const std::string &key, const std::string &name, const Bytes &bytes)
    {
        auto path = safeFile(root, name);
        if (fs::exists(path))
            require(read(path) == bytes, "The exported backup changed: " + name);
        else
            write(path, bytes);
        report["exports"][key] = name;
    };
    if (report.contains("before_sha256"))
    {
        auto bytes = read(reportFile(operation, report, "before.bin"));
        require(hash(bytes) == report.at("before_sha256"), "Backup checksum differs.");
        save("firmware", operation.filename().u8string() + " backup.bin", bytes);
        report["backup_file"] = (root / fs::u8path(report["exports"]["firmware"].get<std::string>())).u8string();
    }
    bool before = report.contains("font_before_sha256");
    if (before || (report.value("font_unchanged", false) && report.value("font_verified", false)))
    {
        auto font = read(reportFile(operation, report, before ? "font_before.bin" : "font_after.bin"));
        require(font.size() == 13824 && hash(font) == report.at(before ? "font_before_sha256" : "font_readback_sha256"),
                "Font backup checksum differs.");
        std::string mcm = "MAX7456\n";
        for (size_t index = 0; index < 256; ++index)
            for (size_t byte = 0; byte < 64; ++byte)
            {
                auto value = byte < 54 ? font[index * 54 + byte] : 0;
                for (int bit = 7; bit >= 0; --bit)
                    mcm += (value & (1 << bit)) ? '1' : '0';
                mcm += '\n';
            }
        auto name = operation.filename().u8string();
        name = name.substr(0, name.find(' ')) + " OSD font backup.mcm";
        save("font", name, Bytes(mcm.begin(), mcm.end()));
    }
}

inline Json latestRecord(const fs::path &root, bool ui)
{
    std::vector<fs::path> files;
    if (fs::exists(root))
        for (auto &entry : fs::directory_iterator(root))
        {
            if (!entry.is_directory() || fs::is_symlink(entry.symlink_status()))
                continue;
            auto directory = entry.path();
            if (fs::exists(directory / L"Data/session.json"))
                directory /= L"Data";
            if (ui)
            {
                if (fs::exists(directory / L"session.json"))
                    files.push_back(directory / L"ui.json");
            }
            else
            {
                files.push_back(directory / L"report.json");
                if (fs::exists(directory / L"session.json"))
                    for (auto &operation : fs::directory_iterator(directory))
                        if (operation.is_directory() && !fs::is_symlink(operation.symlink_status()))
                            files.push_back(operation.path() / L"report.json");
            }
        }
    Json latest;
    std::string time;
    fs::file_time_type modified{};
    for (auto &file : files)
    {
        if (!fs::is_regular_file(file))
            continue;
        auto record = jsonRead(file);
        auto updated = record.value("updated_utc", "");
        auto written = fs::last_write_time(file);
        if (latest.is_null() || updated > time || (updated == time && written > modified))
        {
            latest = std::move(record);
            time = updated;
            modified = written;
        }
    }
    auto legacy = root / (ui ? L"ui.json" : L"latest.json");
    return latest.is_null() && fs::exists(legacy) ? jsonRead(legacy) : latest;
}

inline Json latestReport(const fs::path &root)
{
    return latestRecord(root, false);
}

struct AppSession
{
    fs::path root, folder;

    fs::path data() const { return folder / L"Data"; }

    void start(const fs::path &directory)
    {
        root = directory;
        fs::create_directories(root);
        SYSTEMTIME local;
        GetLocalTime(&local);
        char date[32];
        snprintf(date, sizeof(date), "%04u-%02u-%02u %02u-%02u-%02u", local.wYear, local.wMonth,
                 local.wDay, local.wHour, local.wMinute, local.wSecond);
        for (unsigned attempt = 1;; ++attempt)
        {
            auto name = std::string(date);
            if (attempt > 1)
                name += " (" + std::to_string(attempt) + ")";
            folder = root / fs::u8path(name);
            if (fs::create_directory(folder))
                break;
        }
        fs::create_directory(data());
        jsonWrite(data() / L"session.json", {{"schema", 3},
                                             {"created_utc", stamp()},
                                             {"local_time", date},
                                             {"pid", GetCurrentProcessId()}});
    }

    fs::path nextOperation(const std::string &key) const
    {
        require(!folder.empty() && fs::exists(data() / L"session.json"), "Session folder missing.");
        unsigned number = 1;
        for (auto &entry : fs::directory_iterator(data()))
        {
            auto name = entry.path().filename().u8string();
            auto space = name.find(' ');
            if (entry.is_directory() && space != std::string::npos && space > 0 &&
                name.substr(0, space).find_first_not_of("0123456789") == std::string::npos)
                number = std::max(number, unsigned(std::stoul(name.substr(0, space))) + 1);
        }
        for (;; ++number)
        {
            std::ostringstream name;
            name << std::setw(2) << std::setfill('0') << number << " " << boardLabel(key);
            auto destination = data() / fs::u8path(name.str());
            if (fs::create_directory(destination))
                return destination;
        }
    }

    Json resume(const Json &saved) const
    {
        auto source = reportFolder(root, saved);
        auto destination = nextOperation(saved.at("profile"));
        for (auto &entry : fs::directory_iterator(source))
        {
            require(entry.is_regular_file() && !fs::is_symlink(entry.symlink_status()),
                    "Unexpected file in the saved operation.");
            if (entry.path().filename() != L"report.json")
                fs::copy_file(entry.path(), destination / entry.path().filename());
        }
        auto report = saved;
        report["resumed_from"] = source.u8string();
        report["session"] = folder.u8string();
        report["operation_folder"] = destination.u8string();
        report.erase("exports");
        if (report.contains("backup_file"))
            report["backup_file"] = reportFile(destination, report, "before.bin").u8string();
        report["updated_utc"] = stamp();
        exportBackups(destination, report);
        jsonWrite(destination / L"report.json", report);
        return report;
    }
};
} // namespace drl
