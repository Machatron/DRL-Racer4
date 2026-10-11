#include "../src/platform.hpp"
#include <iostream>

using namespace drl;

int main()
{
    try
    {
        auto tool = cube();
        auto root = tool.parent_path().parent_path();
        require(tool == exeDir() / L"tools/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe",
                "Programmer must come from the flasher folder.");
        for (auto file : {L"Data_Base/STM32_Prog_DB_0x413.xml",
                          L"Data_Base/STM32_Prog_DB_0x452.xml",
                          L"Drivers/STM32Bootloader.inf", L"Drivers/stm32bootloader.cat",
                          L"licenses/STM32CubeProgrammer.txt", L"NOTICE.txt"})
            require(fs::is_regular_file(root / file), "Missing runtime file.");

        auto log = exeDir() / L"runtime_help.log";
        fs::remove(log);
        auto help = process(tool, {L"--help"}, log);
        require(help.find("STM32CubeProgrammer v2.22.0") != std::string::npos,
                "Unexpected programmer version.");
        require(help.find("--upload") != std::string::npos &&
                    help.find("--verify") != std::string::npos,
                "Programmer help is incomplete.");
        fs::remove(log);
        std::cout << "Bundled programmer checks passed\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
