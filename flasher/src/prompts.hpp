#pragma once
#include <string>

namespace drl
{
enum class Step
{
    Idle,
    Connect,
    Reset,
    Verify,
    Font,
    Complete
};

inline std::string stepText(Step step, const std::string &kind, bool restoring = false)
{
    switch (step)
    {
    case Step::Idle:
        return restoring ? "Restore" : "Flash";
    case Step::Connect:
        if (kind == "fc")
            return "Connect Betaflight USB\nwith battery power disconnected";
        if (kind == "ec")
            return "Connect EC USB\nwith battery power disconnected";
        return "Connect to LEDC USB JST\nand connect battery power";
    case Step::Reset:
        return "Double press EC RESET";
    case Step::Verify:
        if (kind == "fc")
            return "Connect Betaflight USB in bootloader mode";
        if (kind == "ec")
            return "Double press EC RESET";
        return "Disconnect battery power for 5 seconds\nand reconnect";
    case Step::Font:
        if (kind == "ec")
            return "Connect battery power\nLeave EC USB connected";
        return {};
    case Step::Complete:
        return kind == "ec" ? "Complete\nPress EC RESET once" : "Complete";
    }
    return {};
}
} // namespace drl
