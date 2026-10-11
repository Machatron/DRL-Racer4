#include "../src/platform.hpp"
#include <cassert>
#include <iostream>

using namespace drl;

int main()
{
    try
    {
        auto tool = cube();
        auto root = tool.parent_path().parent_path();
        assert(tool == exeDir() / L"tools/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe");
        for (auto file :
             {L"Data_Base/STM32_Prog_DB_0x413.xml", L"Data_Base/STM32_Prog_DB_0x452.xml",
              L"Drivers/STM32Bootloader.inf", L"Drivers/stm32bootloader.cat",
              L"licenses/STM32CubeProgrammer.txt", L"NOTICE.txt"})
            assert(fs::is_regular_file(root / file));

        auto log = exeDir() / L"runtime_help.log";
        fs::remove(log);
        auto help = process(tool, {L"--help"}, log);
        assert(help.find("STM32CubeProgrammer v2.22.0") != std::string::npos);
        assert(help.find("--upload") != std::string::npos &&
               help.find("--verify") != std::string::npos);
        fs::remove(log);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
