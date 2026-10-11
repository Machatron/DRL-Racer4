#pragma once
#include "recovery.hpp"
#include <thread>

namespace drl
{
class SerialUSB
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::string connector;

  public:
    explicit SerialUSB(const std::wstring &port, const std::string &device = "EC USB")
        : connector(device)
    {
        handle = CreateFileW((L"\\\\.\\" + port).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        require(handle != INVALID_HANDLE_VALUE,
                "Close Configurator and connect only " + connector + ".");
        try
        {
            DCB d{};
            d.DCBlength = sizeof(d);
            require(GetCommState(handle, &d), "Cannot read serial settings.");
            d.BaudRate = 115200;
            d.ByteSize = 8;
            d.Parity = NOPARITY;
            d.StopBits = ONESTOPBIT;
            d.fBinary = TRUE;
            d.fParity = FALSE;
            d.fDtrControl = DTR_CONTROL_ENABLE;
            d.fRtsControl = RTS_CONTROL_DISABLE;
            d.fOutxCtsFlow = d.fOutxDsrFlow = d.fOutX = d.fInX = FALSE;
            require(SetCommState(handle, &d), "Cannot configure serial port.");
            COMMTIMEOUTS timeout = {20, 0, 80, 0, 500};
            require(SetCommTimeouts(handle, &timeout), "Cannot set serial timeout.");
            PurgeComm(handle, PURGE_RXCLEAR | PURGE_TXCLEAR);
        }
        catch (...)
        {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            throw;
        }
    }
    SerialUSB(const SerialUSB &) = delete;
    ~SerialUSB()
    {
        CloseHandle(handle);
    }
    DWORD transfer(void *data, DWORD size, bool writing)
    {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        require(operation.hEvent != nullptr, "Cannot create serial event.");
        DWORD transferred = 0;
        auto ok = writing ? WriteFile(handle, data, size, nullptr, &operation)
                          : ReadFile(handle, data, size, nullptr, &operation);
        auto error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && error == ERROR_IO_PENDING)
        {
            auto result = WaitForSingleObject(operation.hEvent, 1500);
            if (result != WAIT_OBJECT_0)
                CancelIoEx(handle, &operation);
            ok = GetOverlappedResult(handle, &operation, &transferred, TRUE);
        }
        else if (ok)
            ok = GetOverlappedResult(handle, &operation, &transferred, TRUE);
        CloseHandle(operation.hEvent);
        require(ok, "USB transfer failed. Reconnect " + connector + " and retry.");
        return transferred;
    }
    void send(const Bytes &bytes)
    {
        require(transfer(const_cast<uint8_t *>(bytes.data()), DWORD(bytes.size()), true) ==
                    bytes.size(),
                "Incomplete USB write.");
    }
};

template <class Read, class Write, class Progress>
Bytes programFont(const Bytes &expected, Read readGlyph, Write writeGlyph, Progress progress,
                  const Bytes &installed = {}, std::function<void(unsigned)> verification = {})
{
    require(expected.size() == 256 * 54, "Wrong font size.");
    require(installed.empty() || installed.size() == expected.size(), "Wrong font readback size.");
    for (unsigned index = 0; index < 256; ++index)
    {
        Bytes wanted(expected.begin() + index * 54, expected.begin() + (index + 1) * 54);
        for (unsigned attempt = 0;; ++attempt)
        {
            try
            {
                auto current = attempt == 0 && !installed.empty()
                                   ? Bytes(installed.begin() + index * 54,
                                           installed.begin() + (index + 1) * 54)
                                   : readGlyph(index);
                if (current == wanted)
                    break;
                writeGlyph(index, wanted);
                require(readGlyph(index) == wanted, "Font character verification failed.");
                break;
            }
            catch (...)
            {
                if (attempt == 2)
                    throw;
            }
        }
        progress(index + 1);
    }
    Bytes actual;
    for (unsigned index = 0; index < 256; ++index)
    {
        auto data = readGlyph(index);
        actual.insert(actual.end(), data.begin(), data.end());
        if (verification)
            verification(index + 1);
    }
    require(actual == expected, "Complete font readback differs.");
    return actual;
}
} // namespace drl
