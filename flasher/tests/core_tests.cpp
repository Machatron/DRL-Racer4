#include "../src/core.hpp"
#include <iostream>
using namespace drl;
int checks = 0;
void check(bool b, const char *s)
{
    require(b, s);
    checks++;
}
template <class F> void rejects(F f)
{
    bool caught = false;
    try
    {
        f();
    }
    catch (...)
    {
        caught = true;
    }
    check(caught, "Expected rejection");
}
int main(int argc, char **argv)
{
    try
    {
        require(argc == 3, "Arguments: source bundle");
        fs::path r = fs::u8path(argv[1]), p = fs::u8path(argv[2]);
        auto c = jsonRead(p / "bundle.json");
        check(hash(Bytes{'a', 'b', 'c'}) ==
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
              "SHA-256");
        check(detectFC("Device ID              : 0x0452\r\n", c) == "FC_F7",
              "F7 detected from DFU listing");
        check(detectFC("Device ID              : 0x0413\r\n", c) == "FC_F4",
              "F4 detected from DFU listing");
        rejects([&] { detectFC("Device ID : 0x999", c); });
        rejects([&] { detectFC("Device ID : 0x452extra", c); });
        rejects([&] { detectFC("Device ID\r\nSerial number: 0x452", c); });
        rejects([&] { detectFC("Device ID : 0x452\nDevice ID : 0x413", c); });
        auto ambiguous = c;
        ambiguous["profiles"]["FC_duplicate"] = c.at("profiles").at("FC_F7");
        rejects([&] { detectFC("Device ID : 0x452", ambiguous); });
        Json pendingFC = {{"profile", "FC_F4"}, {"requested_profile", "FC_AUTO"}};
        check(matchesPendingProfile("FC_AUTO", pendingFC), "Detected FC recovery");
        check(matchesPendingProfile("FC_F4", pendingFC), "Manual FC recovery after detection");
        check(!matchesPendingProfile("FC_F7", pendingFC), "Other FC recovery rejected");
        pendingFC["requested_profile"] = "FC_F4";
        check(matchesPendingProfile("FC_AUTO", pendingFC), "Detection resumes a manual FC session");
        check(!matchesPendingProfile("EC_F4", pendingFC), "EC does not resume an FC session");
        check(!matchesPendingProfile("EC_AUTO", pendingFC), "EC detection does not resume an FC session");
        for (auto key : {"EC_F4", "EC_F7"})
        {
            Json pendingEC = {{"profile", key}, {"requested_profile", key}};
            check(matchesPendingProfile("EC_AUTO", pendingEC), "EC detection resumes older sessions");
            pendingEC["requested_profile"] = "EC_AUTO";
            check(matchesPendingProfile("EC_AUTO", pendingEC), "EC detection resumes its own session");
            check(!matchesPendingProfile("FC_AUTO", pendingEC), "FC does not resume an EC session");
        }
        Bytes full(0x80000);
        for (size_t i = 0; i < full.size(); i++)
            full[i] = uint8_t(i * 37);
        Bytes uf(2048 * 512);
        for (size_t i = 0; i < 2048; i++)
        {
            size_t a = i * 512;
            put(uf, a, 0x0a324655);
            put(uf, a + 4, 0x9e5d5157);
            put(uf, a + 12, i * 256);
            put(uf, a + 16, 256);
            put(uf, a + 20, i);
            put(uf, a + 24, 2048);
            put(uf, a + 508, 0x0ab16f30);
            std::copy_n(full.begin() + i * 256, 256, uf.begin() + a + 32);
        }
        check(decodeUF2(uf) == full, "UF2 full roundtrip");
        auto bad = uf;
        put(bad, 512 + 12, 0);
        rejects([&] { decodeUF2(bad); });
        bad = uf;
        bad[508] = 0;
        rejects([&] { decodeUF2(bad); });
        bad = uf;
        bad.resize(512);
        rejects([&] { decodeUF2(bad); });
        auto app = encodeUF2(full);
        check(app.size() == 1984 * 512, "Application length");
        for (size_t i = 0; i < 1984; i++)
            check(u32(app, i * 512 + 12) >= 0x4000, "Bootloader excluded");
        for (auto &entry : c["profiles"].items())
        {
            auto &profile = entry.value();
            auto image = verifiedFile(p, profile["image"]);
            check(!image.empty(), "Image present");
            if (profile["kind"] == "fc")
            {
                auto b =
                    read(r / "baselines" / (entry.key() == "FC_F7" ? "fc_f7.bin" : "fc_f4.bin"));
                std::string info = "Device ID : " + profile.at("device_id").get<std::string>();
                validateFC(info, profile, b.size());
                rejects([&] { validateFC("Device ID : 0x999", profile, b.size()); });
                rejects([&] { validateFC("No device found", profile, b.size()); });
                rejects([&] { validateFC(info, profile, b.size() / 2); });
                check(!profile.contains("known"), "FC firmware versions are unrestricted");
                auto merged = fcExpected(b, image, false);
                check(std::equal(b.begin() + 0x4000, b.begin() + 0x8000, merged.begin() + 0x4000),
                      "FC settings preserved");
                auto reset = fcExpected(b, image, true);
                check(std::all_of(reset.begin() + 0x4000, reset.begin() + 0x8000,
                                  [](auto v) { return v == 255; }),
                      "FC defaults reset");
                auto unknown = b;
                unknown[0x9000] ^= 0x5a;
                unknown[0x10000] ^= 0xc3;
                validateFC(info, profile, unknown.size());
                check(fcExpected(unknown, image, true) == reset,
                      "Unknown FC firmware can be replaced on the selected hardware");
                auto damaged = image;
                damaged[12] = 'Z';
                rejects([&] { parseHEX(damaged, b.size()); });
            }
            else
            {
                std::string name =
                    profile["kind"] == "ec"
                        ? (entry.key() == "EC_F4" ? "ec_v21.bin" : "ec_v23.bin")
                        : (entry.key() == "LED_AB" ? "ledc_v22.bin" : "ledc_v21.bin");
                auto b = read(r / "baselines" / name);
                auto k = identify(b, profile);
                check(k["migration"].size() ==
                          (profile["kind"] == "ec" ? (entry.key() == "EC_F4" ? 20 : 22) : 20),
                      "All migration fields present");
                for (auto &m : k["migration"])
                {
                    size_t a = m[0], n = m[2];
                    for (size_t i = 0; i < n; i++)
                        b[a + i] ^= 0x12;
                }
                check(!identify(b, profile).empty(), "Live settings independent of identity");
                auto merged = mergeSAMD(b, image, k, false);
                check(std::equal(b.begin(), b.begin() + 0x4000, merged.begin()),
                      "Bootloader preserved");
                for (auto &m : k["migration"])
                {
                    size_t a = m[0], d = m[1], n = m[2];
                    check(std::equal(b.begin() + a, b.begin() + a + n, merged.begin() + d),
                          "Fresh migration bytes");
                }
                check(!identify(merged, profile).empty(), "Migrated firmware recognized");
                b[0x7000] ^= 1;
                rejects([&] { identify(b, profile); });
            }
        }
        const size_t huge = size_t(-1);
        rejects([&] { u32(Bytes(8), huge); });
        rejects([&] { fcExpected(Bytes(16), Bytes{}, false); });
        Json invalid = {{"migration", {{0x4000, 0x4000, 1}}}};
        Bytes samd(0x80000, 255);
        invalid["clear"] = {{huge, 2}};
        rejects([&] { mergeSAMD(samd, samd, invalid, false); });
        invalid.erase("clear");
        invalid["migration"] = {{huge, 0x4000, 2}};
        rejects([&] { mergeSAMD(samd, samd, invalid, false); });
        invalid["migration"] = {{0x4000, huge, 2}};
        rejects([&] { mergeSAMD(samd, samd, invalid, false); });
        invalid["migration"] = {{0x4000, 0x4000, huge}};
        rejects([&] { mergeSAMD(samd, samd, invalid, false); });
        rejects([&] { safeFile(p, "C:relative.bin"); });
        rejects([&] { safeFile(p, "image.bin:stream"); });
        rejects([&] { safeFile(p, "../outside.bin"); });
        rejects([&] { safeFile(p, "C:/outside.bin"); });
        std::vector<Bytes> ecImages;
        for (auto name : {"ec_v21.bin", "ec_v23.bin", "ec_v25.bin"})
            ecImages.push_back(read(r / "baselines" / name));
        for (auto key : {"EC_F4", "EC_F7"})
            ecImages.push_back(verifiedFile(p, c["profiles"][key]["image"]));
        for (auto before : ecImages)
        {
            auto key = detectEC(before, c, p);
            auto profile = c["profiles"][key];
            auto known = identify(before, profile);
            before[0x100] ^= 0x5a;
            for (auto &m : known["migration"])
                before.at(m[0].get<size_t>()) ^= 0x12;
            check(detectEC(before, c, p) == key, "EC detection ignores live settings and bootloader");
            auto expected = mergeSAMD(before, verifiedFile(p, profile["image"]), known, false);
            check(std::equal(before.begin(), before.begin() + 0x4000, expected.begin()),
                  "Detected EC bootloader preserved");
            for (auto other : {"EC_F4", "EC_F7"})
            {
                auto candidate = c["profiles"][other];
                check(mergeSAMD(before, verifiedFile(p, candidate["image"]),
                                identify(before, candidate), false) == expected,
                      "Both EC packages produce the same migrated image");
            }
            auto upgradedKey = detectEC(expected, c, p);
            auto upgraded = c["profiles"][upgradedKey];
            check(mergeSAMD(expected, verifiedFile(p, upgraded["image"]),
                            identify(expected, upgraded), false) == expected,
                  "Detected EC can be reflashed without changing settings");
        }
        auto ec = ecImages.front();
        auto conflicting = c;
        for (auto &known : conflicting["profiles"]["EC_F7"]["known"])
            known["clear"] = {{0x4000, 4}};
        rejects([&] { detectEC(ec, conflicting, p); });
        auto damagedEC = c;
        damagedEC["profiles"]["EC_F4"]["image"]["sha256"] = std::string(64, '0');
        rejects([&] { detectEC(ec, damagedEC, p); });
        rejects([&] { detectEC(read(r / "baselines/ledc_v22.bin"), c, p); });
        ec[0x7000] ^= 1;
        rejects([&] { detectEC(ec, c, p); });
        auto future = c;
        for (auto key : {"EC_F4", "EC_F7"})
        {
            auto known = identify(ecImages.front(), c["profiles"][key]);
            known["name"] = "Future EC build";
            known["hash"] = maskedHash(ec, known["mask"]);
            future["profiles"][key]["known"].push_back(known);
        }
        check(!detectEC(ec, future, p).empty(), "New EC catalog entries can be detected");
        for (auto name : {"ledc_v20.bin", "ledc_v21.bin", "ledc_v22.bin"})
        {
            auto old = read(r / "baselines" / name);
            auto key = detectLED(old, c);
            check(key == (std::string(name) == "ledc_v21.bin" ? "LED_C" : "LED_AB"),
                  "Original firmware auto detection");
            auto profile = c["profiles"][key];
            auto k = identify(old, profile);
            check(k["migration"].size() == 20, "Twenty original settings");
            auto target = verifiedFile(p, profile["image"]);
            auto upgraded = mergeSAMD(old, target, k, false);
            check(detectLED(upgraded, c) == key, "Consolidated firmware detection after migration");
            for (auto &m : k["migration"])
            {
                size_t a = m[0], d = m[1], n = m[2];
                check(std::equal(old.begin() + a, old.begin() + a + n, upgraded.begin() + d),
                      "Original field migration");
            }
            check(std::all_of(upgraded.begin() + 0x2a001, upgraded.begin() + 0x2c000,
                              [](auto b) { return b == 255; }),
                  "Original has no inherited selection journal");
            check(std::equal(target.begin() + 0x4a000, target.end(), upgraded.begin() + 0x4a000),
                  "Code and GPIO tables come from selected release");
            auto custom = upgraded;
            custom[0x2a010] = 0x51;
            auto record = identify(custom, profile);
            auto again = mergeSAMD(custom, target, record, false);
            check(again[0x2a010] == 0x51, "Consolidated selection journal retained");
            auto wrong = c;
            wrong["profiles"][key == "LED_AB" ? "LED_C" : "LED_AB"]["known"].push_back(k);
            rejects([&] { detectLED(old, wrong); });
            old[0x7000] ^= 1;
            rejects([&] { detectLED(old, c); });
        }
        Json result = {{"checks", checks}, {"passed", true}, {"hardware_accessed", false}};
        jsonWrite(p.parent_path().parent_path() / "test-results.json", result);
        std::cout << result.dump() << "\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
