#include "../src/core.hpp"
#include <cassert>
#include <iostream>
using namespace drl;
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
    assert(caught);
}
int main(int argc, char **argv)
{
    try
    {
        assert(argc == 3);
        fs::path r = fs::u8path(argv[1]), p = fs::u8path(argv[2]);
        auto c = jsonRead(p / "bundle.json");
        assert(detectFC("Device ID              : 0x0452\r\n", c) == "FC_F7");
        assert(detectFC("Device ID              : 0x0413\r\n", c) == "FC_F4");
        rejects([&] { detectFC("Device ID : 0x999", c); });
        rejects([&] { detectFC("Device ID : 0x452extra", c); });
        rejects([&] { detectFC("Device ID\r\nSerial number: 0x452", c); });
        rejects([&] { detectFC("Device ID : 0x452\nDevice ID : 0x413", c); });
        auto ambiguous = c;
        ambiguous["profiles"]["FC_duplicate"] = c.at("profiles").at("FC_F7");
        rejects([&] { detectFC("Device ID : 0x452", ambiguous); });
        Json pendingFC = {{"profile", "FC_F4"}, {"requested_profile", "FC_AUTO"}};
        assert(matchesPendingProfile("FC_AUTO", pendingFC));
        assert(matchesPendingProfile("FC_F4", pendingFC));
        assert(!matchesPendingProfile("FC_F7", pendingFC));
        pendingFC["requested_profile"] = "FC_F4";
        assert(matchesPendingProfile("FC_AUTO", pendingFC));
        assert(!matchesPendingProfile("EC_F4", pendingFC));
        assert(!matchesPendingProfile("EC_AUTO", pendingFC));
        for (auto key : {"EC_F4", "EC_F7"})
        {
            Json pendingEC = {{"profile", key}, {"requested_profile", key}};
            assert(matchesPendingProfile("EC_AUTO", pendingEC));
            pendingEC["requested_profile"] = "EC_AUTO";
            assert(matchesPendingProfile("EC_AUTO", pendingEC));
            assert(!matchesPendingProfile("FC_AUTO", pendingEC));
        }
        assert(matchesPendingProfile("LED_AUTO",
                                     Json{{"profile", "LED_AB"}, {"requested_profile", "LED_AB"}}));
        assert(!matchesPendingProfile("LED_C", Json{{"profile", "LED_AB"}}));
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
        assert(decodeUF2(uf) == full);
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
        assert(app.size() == 1984 * 512);
        for (size_t i = 0; i < 1984; i++)
            assert(u32(app, i * 512 + 12) >= 0x4000);
        for (auto key : {"FC_F4", "FC_F7"})
        {
            auto &profile = c["profiles"][key];
            auto image = verifiedFile(p, profile["image"]);
            auto b =
                read(r / "baselines" / (key == std::string("FC_F7") ? "fc_f7.bin" : "fc_f4.bin"));
            std::string info = "Device ID : " + profile.at("device_id").get<std::string>();
            validateFC(info, profile, b.size());
            rejects([&] { validateFC("Device ID : 0x999", profile, b.size()); });
            rejects([&] { validateFC("No device found", profile, b.size()); });
            rejects([&] { validateFC(info, profile, b.size() / 2); });
            auto merged = fcExpected(b, image, false);
            assert(std::equal(b.begin() + 0x4000, b.begin() + 0x8000, merged.begin() + 0x4000));
            auto reset = fcExpected(b, image, true);
            assert(std::all_of(reset.begin() + 0x4000, reset.begin() + 0x8000,
                               [](auto v) { return v == 255; }));
            auto unknown = b;
            unknown[0x9000] ^= 0x5a;
            unknown[0x10000] ^= 0xc3;
            validateFC(info, profile, unknown.size());
            assert(fcExpected(unknown, image, true) == reset);
            auto damaged = image;
            damaged[12] = 'Z';
            rejects([&] { parseHEX(damaged, b.size()); });
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
                for (size_t i = 0; i < m[2].get<size_t>(); ++i)
                    before.at(m[0].get<size_t>() + i) ^= 0x12;
            assert(detectEC(before, c, p) == key);
            auto expected = mergeSAMD(before, verifiedFile(p, profile["image"]), known, false);
            assert(std::equal(before.begin(), before.begin() + 0x4000, expected.begin()));
            for (auto &m : known["migration"])
            {
                size_t a = m[0], d = m[1], n = m[2];
                assert(
                    std::equal(before.begin() + a, before.begin() + a + n, expected.begin() + d));
            }
            for (auto other : {"EC_F4", "EC_F7"})
            {
                auto candidate = c["profiles"][other];
                assert(mergeSAMD(before, verifiedFile(p, candidate["image"]),
                                 identify(before, candidate), false) == expected);
            }
            auto upgradedKey = detectEC(expected, c, p);
            auto upgraded = c["profiles"][upgradedKey];
            assert(mergeSAMD(expected, verifiedFile(p, upgraded["image"]),
                             identify(expected, upgraded), false) == expected);
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
        for (auto name : {"ledc_v20.bin", "ledc_v21.bin", "ledc_v22.bin"})
        {
            auto old = read(r / "baselines" / name);
            auto key = detectLED(old, c);
            assert(key == (std::string(name) == "ledc_v21.bin" ? "LED_C" : "LED_AB"));
            auto profile = c["profiles"][key];
            auto k = identify(old, profile);
            old[0x100] ^= 0x5a;
            for (auto &m : k["migration"])
                for (size_t i = 0; i < m[2].get<size_t>(); ++i)
                    old.at(m[0].get<size_t>() + i) ^= 0x12;
            assert(detectLED(old, c) == key);
            auto target = verifiedFile(p, profile["image"]);
            auto upgraded = mergeSAMD(old, target, k, false);
            assert(detectLED(upgraded, c) == key);
            assert(std::equal(old.begin(), old.begin() + 0x4000, upgraded.begin()));
            for (auto &m : k["migration"])
            {
                size_t a = m[0], d = m[1], n = m[2];
                assert(std::equal(old.begin() + a, old.begin() + a + n, upgraded.begin() + d));
            }
            assert(std::all_of(upgraded.begin() + 0x2a001, upgraded.begin() + 0x2c000,
                               [](auto b) { return b == 255; }));
            assert(std::equal(target.begin() + 0x4a000, target.end(), upgraded.begin() + 0x4a000));
            auto custom = upgraded;
            custom[0x2a010] = 0x51;
            auto record = identify(custom, profile);
            auto again = mergeSAMD(custom, target, record, false);
            assert(again[0x2a010] == 0x51);
            auto wrong = c;
            wrong["profiles"][key == "LED_AB" ? "LED_C" : "LED_AB"]["known"].push_back(k);
            rejects([&] { detectLED(old, wrong); });
            old[0x7000] ^= 1;
            rejects([&] { detectLED(old, c); });
        }
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
