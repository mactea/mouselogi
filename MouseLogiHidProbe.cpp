#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <conio.h>

#include <atomic>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
    struct HidDevice
    {
        int index = 0;
        std::wstring path;
        USHORT vendorId = 0;
        USHORT productId = 0;
        USHORT version = 0;
        USAGE usagePage = 0;
        USAGE usage = 0;
        USHORT inputReportLength = 0;
        USHORT outputReportLength = 0;
        USHORT featureReportLength = 0;
        bool canOpenForRead = false;
        DWORD readOpenError = 0;
    };

    std::mutex g_consoleMutex;
    HANDLE g_stopEvent = NULL;
    std::atomic<bool> g_stop(false);

    std::wstring ToLower(std::wstring value)
    {
        for (size_t i = 0; i < value.size(); ++i)
        {
            if (value[i] >= L'A' && value[i] <= L'Z')
            {
                value[i] = static_cast<wchar_t>(value[i] - L'A' + L'a');
            }
        }
        return value;
    }

    bool ContainsLogitechVid(const std::wstring& path)
    {
        std::wstring lower = ToLower(path);
        return lower.find(L"vid_046d") != std::wstring::npos ||
               lower.find(L"vid&02046d") != std::wstring::npos ||
               lower.find(L"ven_046d") != std::wstring::npos;
    }

    std::wstring HexWord(USHORT value)
    {
        std::wstringstream stream;
        stream << L"0x" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << value;
        return stream.str();
    }

    std::wstring HexDword(DWORD value)
    {
        std::wstringstream stream;
        stream << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << value;
        return stream.str();
    }

    std::wstring LastErrorText(DWORD error)
    {
        switch (error)
        {
            case 0:
                return L"OK";
            case ERROR_ACCESS_DENIED:
                return L"Access denied";
            case ERROR_SHARING_VIOLATION:
                return L"Sharing violation";
            case ERROR_FILE_NOT_FOUND:
                return L"File not found";
            case ERROR_INVALID_PARAMETER:
                return L"Invalid parameter";
            case ERROR_DEVICE_NOT_CONNECTED:
                return L"Device not connected";
            case ERROR_OPERATION_ABORTED:
                return L"Operation aborted";
            case ERROR_IO_PENDING:
                return L"IO pending";
            default:
                return L"error " + std::to_wstring(error);
        }
    }

    std::wstring BytesToHex(const BYTE* data, DWORD length, DWORD maxBytes)
    {
        std::wstringstream stream;
        DWORD count = length < maxBytes ? length : maxBytes;
        for (DWORD i = 0; i < count; ++i)
        {
            if (i != 0)
            {
                stream << L' ';
            }
            stream << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0') << static_cast<unsigned int>(data[i]);
        }
        if (length > maxBytes)
        {
            stream << L" ...";
        }
        return stream.str();
    }

    std::wstring Timestamp()
    {
        SYSTEMTIME now = {};
        GetLocalTime(&now);

        std::wstringstream stream;
        stream << std::setfill(L'0')
               << std::setw(2) << now.wHour << L":"
               << std::setw(2) << now.wMinute << L":"
               << std::setw(2) << now.wSecond << L"."
               << std::setw(3) << now.wMilliseconds;
        return stream.str();
    }

    void ConsoleWrite(const std::wstring& text)
    {
        HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (output != INVALID_HANDLE_VALUE && output != NULL && GetConsoleMode(output, &mode))
        {
            DWORD written = 0;
            WriteConsoleW(output, text.c_str(), static_cast<DWORD>(text.size()), &written, NULL);
            return;
        }

        std::wcout << text;
    }

    void ConsoleWriteLine(const std::wstring& text)
    {
        ConsoleWrite(text);
        ConsoleWrite(L"\r\n");
    }

    void ConsoleWriteLine()
    {
        ConsoleWrite(L"\r\n");
    }

    bool TryOpenForInfo(const std::wstring& path, HANDLE* handle)
    {
        *handle = CreateFileW(
            path.c_str(),
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        return *handle != INVALID_HANDLE_VALUE;
    }

    bool FillDeviceInfo(const std::wstring& path, HidDevice* device)
    {
        HANDLE handle = INVALID_HANDLE_VALUE;
        if (!TryOpenForInfo(path, &handle))
        {
            return false;
        }

        HIDD_ATTRIBUTES attributes = {};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(handle, &attributes))
        {
            device->vendorId = attributes.VendorID;
            device->productId = attributes.ProductID;
            device->version = attributes.VersionNumber;
        }

        PHIDP_PREPARSED_DATA preparsedData = NULL;
        if (HidD_GetPreparsedData(handle, &preparsedData))
        {
            HIDP_CAPS caps = {};
            if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS)
            {
                device->usagePage = caps.UsagePage;
                device->usage = caps.Usage;
                device->inputReportLength = caps.InputReportByteLength;
                device->outputReportLength = caps.OutputReportByteLength;
                device->featureReportLength = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(preparsedData);
        }

        CloseHandle(handle);
        return true;
    }

    void CheckReadOpen(HidDevice* device)
    {
        HANDLE handle = CreateFileW(
            device->path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);

        if (handle == INVALID_HANDLE_VALUE)
        {
            device->canOpenForRead = false;
            device->readOpenError = GetLastError();
            return;
        }

        device->canOpenForRead = true;
        device->readOpenError = 0;
        CloseHandle(handle);
    }

    std::vector<HidDevice> EnumerateLogitechHidDevices()
    {
        std::vector<HidDevice> devices;

        GUID hidGuid = {};
        HidD_GetHidGuid(&hidGuid);

        HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (deviceInfoSet == INVALID_HANDLE_VALUE)
        {
            return devices;
        }

        for (DWORD index = 0;; ++index)
        {
            SP_DEVICE_INTERFACE_DATA interfaceData = {};
            interfaceData.cbSize = sizeof(interfaceData);

            if (!SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &hidGuid, index, &interfaceData))
            {
                if (GetLastError() == ERROR_NO_MORE_ITEMS)
                {
                    break;
                }
                continue;
            }

            DWORD requiredSize = 0;
            SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, NULL, 0, &requiredSize, NULL);
            if (requiredSize == 0)
            {
                continue;
            }

            std::vector<BYTE> detailBuffer(requiredSize);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, detail, requiredSize, NULL, NULL))
            {
                continue;
            }

            HidDevice device;
            device.path = detail->DevicePath;

            if (!FillDeviceInfo(device.path, &device))
            {
                continue;
            }

            if (device.vendorId != 0x046D && !ContainsLogitechVid(device.path))
            {
                continue;
            }

            CheckReadOpen(&device);
            device.index = static_cast<int>(devices.size()) + 1;
            devices.push_back(device);
        }

        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return devices;
    }

    void PrintDeviceSummary(const std::vector<HidDevice>& devices)
    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);

        std::wcout << L"Logitech HID interfaces:" << std::endl;
        if (devices.empty())
        {
            std::wcout << L"  none found" << std::endl;
            return;
        }

        for (size_t i = 0; i < devices.size(); ++i)
        {
            const HidDevice& device = devices[i];
            std::wcout << L"  [" << device.index << L"] "
                       << L"VID=" << HexWord(device.vendorId)
                       << L" PID=" << HexWord(device.productId)
                       << L" ver=" << HexWord(device.version)
                       << L" usagePage=" << HexWord(device.usagePage)
                       << L" usage=" << HexWord(device.usage)
                       << L" in=" << device.inputReportLength
                       << L" out=" << device.outputReportLength
                       << L" feature=" << device.featureReportLength;

            if (device.canOpenForRead)
            {
                std::wcout << L" read=OK";
            }
            else
            {
                std::wcout << L" read=NO(" << device.readOpenError << L": " << LastErrorText(device.readOpenError) << L")";
            }
            std::wcout << std::endl;

            std::wcout << L"      " << device.path << std::endl;
        }
    }

    void PrintReport(const HidDevice& device, const std::vector<BYTE>& report, DWORD bytesRead)
    {
        std::lock_guard<std::mutex> lock(g_consoleMutex);
        std::wcout << Timestamp()
                   << L" HID [" << device.index << L"]"
                   << L" PID=" << HexWord(device.productId)
                   << L" page=" << HexWord(device.usagePage)
                   << L" usage=" << HexWord(device.usage)
                   << L" len=" << bytesRead
                   << L" data=" << BytesToHex(report.data(), bytesRead, 64)
                   << std::endl;
    }

    void PrintReadError(const HidDevice& device, DWORD error)
    {
        if (g_stop.load())
        {
            return;
        }

        std::lock_guard<std::mutex> lock(g_consoleMutex);
        std::wcout << L"HID [" << device.index << L"] read stopped: "
                   << error << L" " << LastErrorText(error) << std::endl;
    }

    void ReaderThread(HidDevice device)
    {
        if (!device.canOpenForRead || device.inputReportLength == 0)
        {
            return;
        }

        HANDLE handle = CreateFileW(
            device.path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);

        if (handle == INVALID_HANDLE_VALUE)
        {
            return;
        }

        HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (event == NULL)
        {
            CloseHandle(handle);
            return;
        }

        std::vector<BYTE> buffer(device.inputReportLength, 0);
        std::vector<BYTE> previous;

        while (!g_stop.load())
        {
            OVERLAPPED overlapped = {};
            overlapped.hEvent = event;
            ResetEvent(event);

            DWORD bytesRead = 0;
            BOOL ok = ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, &overlapped);
            if (!ok)
            {
                DWORD error = GetLastError();
                if (error == ERROR_IO_PENDING)
                {
                    HANDLE waitHandles[2] = { g_stopEvent, event };
                    DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                    if (waitResult == WAIT_OBJECT_0)
                    {
                        CancelIo(handle);
                        break;
                    }

                    if (waitResult == WAIT_OBJECT_0 + 1)
                    {
                        if (!GetOverlappedResult(handle, &overlapped, &bytesRead, FALSE))
                        {
                            error = GetLastError();
                            if (error != ERROR_OPERATION_ABORTED)
                            {
                                PrintReadError(device, error);
                            }
                            break;
                        }
                    }
                    else
                    {
                        PrintReadError(device, GetLastError());
                        break;
                    }
                }
                else
                {
                    PrintReadError(device, error);
                    break;
                }
            }

            if (bytesRead == 0)
            {
                continue;
            }

            bool changed = previous.size() != bytesRead;
            if (!changed)
            {
                for (DWORD i = 0; i < bytesRead; ++i)
                {
                    if (previous[i] != buffer[i])
                    {
                        changed = true;
                        break;
                    }
                }
            }

            if (changed)
            {
                PrintReport(device, buffer, bytesRead);
                previous.assign(buffer.begin(), buffer.begin() + bytesRead);
            }
        }

        CloseHandle(event);
        CloseHandle(handle);
    }

    BOOL WINAPI ConsoleHandler(DWORD controlType)
    {
        if (controlType == CTRL_C_EVENT || controlType == CTRL_CLOSE_EVENT || controlType == CTRL_BREAK_EVENT)
        {
            g_stop.store(true);
            if (g_stopEvent != NULL)
            {
                SetEvent(g_stopEvent);
            }
            return TRUE;
        }
        return FALSE;
    }

    bool IsHidppInterface(const HidDevice& device)
    {
        return device.vendorId == 0x046D &&
               device.usagePage == 0xFF43 &&
               device.usage == 0x0202 &&
               device.inputReportLength >= 20 &&
               device.outputReportLength >= 20;
    }

    class HidppSession
    {
    public:
        explicit HidppSession(const HidDevice& device)
            : device_(device), handle_(INVALID_HANDLE_VALUE)
        {
        }

        ~HidppSession()
        {
            Close();
        }

        bool Open()
        {
            handle_ = CreateFileW(
                device_.path.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                NULL);
            return handle_ != INVALID_HANDLE_VALUE;
        }

        void Close()
        {
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle_);
                handle_ = INVALID_HANDLE_VALUE;
            }
        }

        bool Request(BYTE deviceIndex, BYTE featureIndex, BYTE functionId, const std::vector<BYTE>& params, std::vector<BYTE>* response)
        {
            if (handle_ == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            std::vector<BYTE> request(device_.outputReportLength, 0);
            request[0] = 0x11; // HID++ long report
            request[1] = deviceIndex;
            request[2] = featureIndex;
            request[3] = static_cast<BYTE>((functionId << 4) | 0x0A); // software id 0xA
            for (size_t i = 0; i < params.size() && 4 + i < request.size(); ++i)
            {
                request[4 + i] = params[i];
            }

            HidD_FlushQueue(handle_);

            if (!WriteReport(request))
            {
                return false;
            }

            DWORD deadline = GetTickCount() + 900;
            while (static_cast<LONG>(deadline - GetTickCount()) > 0)
            {
                std::vector<BYTE> candidate;
                DWORD timeout = deadline - GetTickCount();
                if (!ReadReport(timeout, &candidate))
                {
                    return false;
                }

                if (candidate.size() >= 4 &&
                    candidate[0] == 0x11 &&
                    candidate[1] == deviceIndex &&
                    candidate[2] == featureIndex &&
                    candidate[3] == request[3])
                {
                    *response = candidate;
                    return true;
                }

                // HID++ error response. Byte 4 usually contains the original feature index.
                if (candidate.size() >= 7 &&
                    candidate[0] == 0x11 &&
                    candidate[1] == deviceIndex &&
                    candidate[2] == 0xFF &&
                    candidate[4] == featureIndex &&
                    candidate[5] == request[3])
                {
                    *response = candidate;
                    return true;
                }
            }

            return false;
        }

    private:
        bool WriteReport(const std::vector<BYTE>& report)
        {
            HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (event == NULL)
            {
                return false;
            }

            OVERLAPPED overlapped = {};
            overlapped.hEvent = event;
            DWORD written = 0;
            BOOL ok = WriteFile(handle_, report.data(), static_cast<DWORD>(report.size()), &written, &overlapped);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                DWORD waitResult = WaitForSingleObject(event, 1000);
                if (waitResult == WAIT_OBJECT_0)
                {
                    ok = GetOverlappedResult(handle_, &overlapped, &written, FALSE);
                }
                else
                {
                    CancelIo(handle_);
                    ok = FALSE;
                }
            }

            CloseHandle(event);
            return ok && written == report.size();
        }

        bool ReadReport(DWORD timeoutMs, std::vector<BYTE>* report)
        {
            report->assign(device_.inputReportLength, 0);

            HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (event == NULL)
            {
                return false;
            }

            OVERLAPPED overlapped = {};
            overlapped.hEvent = event;
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(handle_, report->data(), static_cast<DWORD>(report->size()), &bytesRead, &overlapped);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                DWORD waitResult = WaitForSingleObject(event, timeoutMs);
                if (waitResult == WAIT_OBJECT_0)
                {
                    ok = GetOverlappedResult(handle_, &overlapped, &bytesRead, FALSE);
                }
                else
                {
                    CancelIo(handle_);
                    CloseHandle(event);
                    return false;
                }
            }

            CloseHandle(event);
            if (!ok || bytesRead == 0)
            {
                return false;
            }

            report->resize(bytesRead);
            return true;
        }

        HidDevice device_;
        HANDLE handle_;
    };

    bool IsHidppError(const std::vector<BYTE>& response)
    {
        return response.size() >= 7 && response[2] == 0xFF;
    }

    BYTE HidppErrorCode(const std::vector<BYTE>& response)
    {
        if (response.size() >= 7 && response[2] == 0xFF)
        {
            return response[6];
        }
        return 0;
    }

    bool TryRootGetFeature(HidppSession* session, BYTE deviceIndex, USHORT featureId, BYTE* featureIndex, BYTE* featureType)
    {
        std::vector<BYTE> params;
        params.push_back(static_cast<BYTE>((featureId >> 8) & 0xFF));
        params.push_back(static_cast<BYTE>(featureId & 0xFF));

        std::vector<BYTE> response;
        if (!session->Request(deviceIndex, 0x00, 0x00, params, &response))
        {
            return false;
        }

        if (IsHidppError(response))
        {
            return false;
        }

        if (response.size() < 6)
        {
            return false;
        }

        *featureIndex = response[4];
        *featureType = response[5];
        return *featureIndex != 0;
    }

    struct HidppBasics
    {
        BYTE deviceIndex = 0;
        BYTE featureSetIndex = 0;
        BYTE specialKeysIndex = 0;
    };

    struct HidppControl
    {
        USHORT cid = 0;
        USHORT tid = 0;
        BYTE flags = 0;
        BYTE additionalFlags = 0;
        std::vector<BYTE> raw;
    };

    bool HidppRequestParams(HidppSession* session, BYTE deviceIndex, BYTE featureIndex, BYTE functionId, const std::vector<BYTE>& params, std::vector<BYTE>* resultParams)
    {
        std::vector<BYTE> response;
        if (!session->Request(deviceIndex, featureIndex, functionId, params, &response))
        {
            return false;
        }

        if (IsHidppError(response))
        {
            std::lock_guard<std::mutex> lock(g_consoleMutex);
            std::wcout << L"    HID++ error featureIndex=" << HexWord(featureIndex)
                       << L" function=" << static_cast<int>(functionId)
                       << L" code=" << static_cast<int>(HidppErrorCode(response))
                       << L" raw=" << BytesToHex(response.data(), static_cast<DWORD>(response.size()), 64)
                       << std::endl;
            return false;
        }

        if (response.size() <= 4)
        {
            return false;
        }

        resultParams->assign(response.begin() + 4, response.end());
        return true;
    }

    bool GetHidppBasics(HidppSession* session, HidppBasics* basics)
    {
        BYTE featureType = 0;
        const BYTE candidates[] = { 0xFF, 0x01, 0x00, 0x02, 0x03, 0x04, 0x05, 0x06 };
        for (size_t i = 0; i < sizeof(candidates); ++i)
        {
            BYTE candidateFeatureIndex = 0;
            BYTE candidateFeatureType = 0;
            if (TryRootGetFeature(session, candidates[i], 0x0001, &candidateFeatureIndex, &candidateFeatureType))
            {
                basics->deviceIndex = candidates[i];
                basics->featureSetIndex = candidateFeatureIndex;
                break;
            }
        }

        if (basics->deviceIndex == 0 || basics->featureSetIndex == 0)
        {
            return false;
        }

        if (!TryRootGetFeature(session, basics->deviceIndex, 0x1B04, &basics->specialKeysIndex, &featureType))
        {
            basics->specialKeysIndex = 0;
        }

        return basics->specialKeysIndex != 0;
    }

    bool GetHidppControls(HidppSession* session, const HidppBasics& basics, std::vector<HidppControl>* controls)
    {
        std::vector<BYTE> controlParams;
        if (!HidppRequestParams(session, basics.deviceIndex, basics.specialKeysIndex, 0x00, std::vector<BYTE>(), &controlParams) || controlParams.empty())
        {
            return false;
        }

        BYTE controlCount = controlParams[0];
        for (BYTE i = 0; i < controlCount; ++i)
        {
            std::vector<BYTE> infoParams;
            std::vector<BYTE> requestParams;
            requestParams.push_back(i);
            if (!HidppRequestParams(session, basics.deviceIndex, basics.specialKeysIndex, 0x01, requestParams, &infoParams))
            {
                continue;
            }

            HidppControl control;
            control.raw = infoParams;
            control.cid = infoParams.size() >= 2 ? static_cast<USHORT>((infoParams[0] << 8) | infoParams[1]) : 0;
            control.tid = infoParams.size() >= 4 ? static_cast<USHORT>((infoParams[2] << 8) | infoParams[3]) : 0;
            control.flags = infoParams.size() >= 5 ? infoParams[4] : 0;
            control.additionalFlags = infoParams.size() >= 9 ? infoParams[8] : 0;
            if (control.cid != 0)
            {
                controls->push_back(control);
            }
        }

        return !controls->empty();
    }

    bool SetCidReporting(HidppSession* session, const HidppBasics& basics, USHORT cid, bool divert, bool rawXY)
    {
        BYTE reporting = 0x02; // dvalid
        if (divert)
        {
            reporting |= 0x01;
        }

        if (rawXY)
        {
            reporting |= 0x30; // rawXY + rvalid
        }

        std::vector<BYTE> params;
        params.push_back(static_cast<BYTE>((cid >> 8) & 0xFF));
        params.push_back(static_cast<BYTE>(cid & 0xFF));
        params.push_back(reporting);
        params.push_back(0x00);
        params.push_back(0x00);

        std::vector<BYTE> result;
        return HidppRequestParams(session, basics.deviceIndex, basics.specialKeysIndex, 0x03, params, &result);
    }

    const HidDevice* FindHidppDevice(const std::vector<HidDevice>& devices)
    {
        for (size_t i = 0; i < devices.size(); ++i)
        {
            if (IsHidppInterface(devices[i]))
            {
                return &devices[i];
            }
        }
        return NULL;
    }

    void PrintHidppInfo(const std::vector<HidDevice>& devices)
    {
        const HidDevice* hidppDevice = FindHidppDevice(devices);

        if (hidppDevice == NULL)
        {
            std::wcout << L"No HID++ interface found." << std::endl;
            return;
        }

        std::wcout << L"HID++ candidate: [" << hidppDevice->index << L"] PID=" << HexWord(hidppDevice->productId)
                   << L" page=" << HexWord(hidppDevice->usagePage)
                   << L" usage=" << HexWord(hidppDevice->usage)
                   << std::endl;

        HidppSession session(*hidppDevice);
        if (!session.Open())
        {
            std::wcout << L"Could not open HID++ interface for read/write." << std::endl;
            return;
        }

        HidppBasics basics;
        if (!GetHidppBasics(&session, &basics))
        {
            std::wcout << L"HID++ root query did not respond on common device indexes." << std::endl;
            return;
        }

        std::wcout << L"HID++ deviceIndex=" << HexWord(basics.deviceIndex)
                   << L" FEATURE_SET index=" << HexWord(basics.featureSetIndex)
                   << std::endl;

        std::vector<BYTE> params;
        if (!HidppRequestParams(&session, basics.deviceIndex, basics.featureSetIndex, 0x00, std::vector<BYTE>(), &params) || params.empty())
        {
            std::wcout << L"Could not read HID++ feature count." << std::endl;
            return;
        }

        BYTE featureCount = params[0];
        std::wcout << L"HID++ feature count=" << static_cast<int>(featureCount) << std::endl;

        for (BYTE i = 0; i < featureCount; ++i)
        {
            std::vector<BYTE> featureParams;
            std::vector<BYTE> requestParams;
            requestParams.push_back(i);
            if (!HidppRequestParams(&session, basics.deviceIndex, basics.featureSetIndex, 0x01, requestParams, &featureParams) || featureParams.size() < 3)
            {
                continue;
            }

            USHORT featureId = static_cast<USHORT>((featureParams[0] << 8) | featureParams[1]);
            BYTE type = featureParams[2];
            std::wcout << L"  feature[" << std::setw(2) << std::setfill(L' ') << static_cast<int>(i) << L"] id="
                       << HexWord(featureId) << L" type=" << HexWord(type);

            BYTE rootIndex = 0;
            BYTE rootType = 0;
            if (TryRootGetFeature(&session, basics.deviceIndex, featureId, &rootIndex, &rootType))
            {
                std::wcout << L" index=" << HexWord(rootIndex);
            }
            std::wcout << std::endl;
        }

        std::wcout << L"Special Keys / Mouse Buttons feature index=" << HexWord(basics.specialKeysIndex)
                   << std::endl;

        std::vector<HidppControl> controls;
        if (!GetHidppControls(&session, basics, &controls))
        {
            std::wcout << L"Could not read controls." << std::endl;
            return;
        }

        std::wcout << L"Control count=" << controls.size() << std::endl;
        for (size_t i = 0; i < controls.size(); ++i)
        {
            std::wcout << L"  control[" << std::setw(2) << std::setfill(L' ') << static_cast<int>(i) << L"]"
                       << L" cid=" << HexWord(controls[i].cid)
                       << L" tid=" << HexWord(controls[i].tid)
                       << L" flags=" << HexWord(controls[i].flags)
                       << L" addFlags=" << HexWord(controls[i].additionalFlags)
                       << L" raw=" << BytesToHex(controls[i].raw.data(), static_cast<DWORD>(controls[i].raw.size()), 32)
                       << std::endl;
        }
    }

    void DivertAndWatch(const std::vector<HidDevice>& devices)
    {
        const HidDevice* hidppDevice = FindHidppDevice(devices);
        if (hidppDevice == NULL)
        {
            std::wcout << L"No HID++ interface found." << std::endl;
            return;
        }

        HidppBasics basics;
        std::vector<HidppControl> controls;
        std::vector<USHORT> divertedCids;

        {
            HidppSession session(*hidppDevice);
            if (!session.Open())
            {
                std::wcout << L"Could not open HID++ interface for read/write." << std::endl;
                return;
            }

            if (!GetHidppBasics(&session, &basics) || !GetHidppControls(&session, basics, &controls))
            {
                std::wcout << L"Could not initialize HID++ controls." << std::endl;
                return;
            }

            std::wcout << L"Temporarily diverting controls with the divert flag." << std::endl;
            for (size_t i = 0; i < controls.size(); ++i)
            {
                bool divertable = (controls[i].flags & 0x20) != 0;
                bool virtualControl = (controls[i].flags & 0x80) != 0;
                bool rawXYCapable = (controls[i].additionalFlags & 0x01) != 0;
                if (!divertable || virtualControl)
                {
                    continue;
                }

                if (SetCidReporting(&session, basics, controls[i].cid, true, rawXYCapable))
                {
                    divertedCids.push_back(controls[i].cid);
                    std::wcout << L"  diverted cid=" << HexWord(controls[i].cid)
                               << L" tid=" << HexWord(controls[i].tid)
                               << (rawXYCapable ? L" rawXY" : L"")
                               << std::endl;
                }
            }
        }

        if (divertedCids.empty())
        {
            std::wcout << L"No controls were diverted." << std::endl;
            return;
        }

        std::wcout << std::endl;
        std::wcout << L"Now press the buttons that were previously invisible." << std::endl;
        std::wcout << L"Watch for HID reports containing cid values. Press Esc or Ctrl+C to stop." << std::endl;
        std::wcout << std::endl;

        std::thread reader(ReaderThread, *hidppDevice);
        while (!g_stop.load())
        {
            if (_kbhit())
            {
                int key = _getch();
                if (key == 27)
                {
                    g_stop.store(true);
                    SetEvent(g_stopEvent);
                    break;
                }
            }
            Sleep(50);
        }

        g_stop.store(true);
        SetEvent(g_stopEvent);
        if (reader.joinable())
        {
            reader.join();
        }

        std::wcout << L"Restoring diverted controls." << std::endl;
        HidppSession restoreSession(*hidppDevice);
        if (restoreSession.Open())
        {
            for (size_t i = 0; i < divertedCids.size(); ++i)
            {
                SetCidReporting(&restoreSession, basics, divertedCids[i], false, false);
            }
        }
    }

    struct GuideStep
    {
        const wchar_t* name;
        const wchar_t* instruction;
    };

    struct GuideResult
    {
        const wchar_t* name;
        bool detected = false;
        USHORT cid = 0;
        std::vector<BYTE> report;
    };

    bool WaitForGuideCid(const HidDevice& device, const HidppBasics& basics, DWORD timeoutMs, USHORT* cid, std::vector<BYTE>* report)
    {
        HANDLE handle = CreateFileW(
            device.path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);

        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        HidD_FlushQueue(handle);

        HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (event == NULL)
        {
            CloseHandle(handle);
            return false;
        }

        DWORD deadline = GetTickCount() + timeoutMs;
        std::vector<BYTE> buffer(device.inputReportLength, 0);
        bool found = false;

        while (!g_stop.load() && static_cast<LONG>(deadline - GetTickCount()) > 0)
        {
            OVERLAPPED overlapped = {};
            overlapped.hEvent = event;
            ResetEvent(event);

            DWORD bytesRead = 0;
            BOOL ok = ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, &overlapped);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                DWORD remaining = deadline - GetTickCount();
                HANDLE waitHandles[2] = { g_stopEvent, event };
                DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, remaining);
                if (waitResult == WAIT_OBJECT_0)
                {
                    CancelIo(handle);
                    break;
                }
                if (waitResult == WAIT_OBJECT_0 + 1)
                {
                    ok = GetOverlappedResult(handle, &overlapped, &bytesRead, FALSE);
                }
                else
                {
                    CancelIo(handle);
                    break;
                }
            }

            if (!ok || bytesRead < 6)
            {
                continue;
            }

            if (buffer[0] == 0x11 &&
                buffer[1] == basics.deviceIndex &&
                buffer[2] == basics.specialKeysIndex &&
                buffer[3] == 0x00)
            {
                USHORT candidateCid = static_cast<USHORT>((buffer[4] << 8) | buffer[5]);
                if (candidateCid != 0)
                {
                    *cid = candidateCid;
                    report->assign(buffer.begin(), buffer.begin() + bytesRead);
                    found = true;
                    break;
                }
            }
        }

        CloseHandle(event);
        CloseHandle(handle);
        return found;
    }

    void RunMxMaster4Guide(const std::vector<HidDevice>& devices)
    {
        const HidDevice* hidppDevice = FindHidppDevice(devices);
        if (hidppDevice == NULL)
        {
            ConsoleWriteLine(L"未找到 MX Master 4 的 HID++ 接口。");
            return;
        }

        if (hidppDevice->productId != 0xB042)
        {
            ConsoleWriteLine(L"警告：期望 MX Master 4 PID 0xB042，实际发现 PID=" + HexWord(hidppDevice->productId) + L"。");
        }
        else
        {
            ConsoleWriteLine(L"已检测到 MX Master 4：PID=" + HexWord(hidppDevice->productId) + L"。");
        }

        HidppBasics basics;
        std::vector<HidppControl> controls;
        std::vector<USHORT> divertedCids;

        {
            HidppSession session(*hidppDevice);
            if (!session.Open())
            {
                ConsoleWriteLine(L"无法打开 HID++ 接口进行读写。");
                return;
            }

            if (!GetHidppBasics(&session, &basics) || !GetHidppControls(&session, basics, &controls))
            {
                ConsoleWriteLine(L"无法初始化 HID++ 控制项。");
                return;
            }

            ConsoleWriteLine(L"正在临时启用 HID++ 软件接管模式。");
            for (size_t i = 0; i < controls.size(); ++i)
            {
                bool divertable = (controls[i].flags & 0x20) != 0;
                bool virtualControl = (controls[i].flags & 0x80) != 0;
                bool rawXYCapable = (controls[i].additionalFlags & 0x01) != 0;
                if (!divertable || virtualControl)
                {
                    continue;
                }

                if (SetCidReporting(&session, basics, controls[i].cid, true, rawXYCapable))
                {
                    divertedCids.push_back(controls[i].cid);
                }
            }
        }

        if (divertedCids.empty())
        {
            ConsoleWriteLine(L"没有可接管的控制项。");
            return;
        }

        static const GuideStep steps[] = {
            { L"侧边横向滚轮向左", L"先从侧边横向滚轮开始：请将拇指横向滚轮向左滚动一格；如果不想识别滚轮，按 S 跳过。" },
            { L"侧边横向滚轮向右", L"请将拇指横向滚轮向右滚动一格；如果不想识别滚轮，按 S 跳过。" },
            { L"侧边后退键", L"请按一下侧边 3 个实体按钮里的后退键。" },
            { L"侧边前进键", L"请按一下侧边 3 个实体按钮里的前进键。" },
            { L"侧边第三键 / 官方 Gesture button", L"请按一下侧边 3 个实体按钮里的第三个按钮，也就是官方标注的 Gesture button。" },
            { L"拇指托 Haptic Sense Panel / Actions Ring", L"请按一下或轻触拇指托上的触觉面板，也就是用于打开 Actions Ring 的区域。" },
            { L"顶部滚轮模式切换键", L"可选：请按一下主滚轮后方的滚轮模式切换键；如果不想识别，按 S 跳过。" },
            { L"主滚轮中键", L"可选：请向下按一下主滚轮；如果不想识别，按 S 跳过。" },
        };

        std::vector<GuideResult> results;
        ConsoleWriteLine();
        ConsoleWriteLine(L"MX Master 4 按键引导识别");
        ConsoleWriteLine(L"流程按官方布局识别：侧边横向滚轮、侧边三键、拇指托 Haptic Sense Panel，再到顶部可选键。");
        ConsoleWriteLine(L"每一步都会在你按下 Enter 后，记录第一个 HID++ CID 事件。");
        ConsoleWriteLine(L"按 S 跳过当前步骤；按 Esc 或 Ctrl+C 退出。");
        ConsoleWriteLine();

        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i)
        {
            GuideResult result;
            result.name = steps[i].name;

            std::wstringstream stepLine;
            stepLine << L"步骤 " << (i + 1) << L"/" << (sizeof(steps) / sizeof(steps[0])) << L"：" << steps[i].name;
            ConsoleWriteLine(stepLine.str());
            ConsoleWriteLine(std::wstring(L"  ") + steps[i].instruction);
            ConsoleWriteLine(L"  请先按 Enter，然后在 5 秒内完成这个鼠标动作。S=跳过，Esc=退出。");
            ConsoleWriteLine(L"  等待输入：Enter=开始本步骤，S=跳过，Esc=退出。");

            int key = _getch();
            if (key == 27)
            {
                g_stop.store(true);
                SetEvent(g_stopEvent);
                break;
            }
            if (key == 's' || key == 'S')
            {
                ConsoleWriteLine(L"  已跳过");
                ConsoleWriteLine();
                results.push_back(result);
                continue;
            }

            USHORT cid = 0;
            std::vector<BYTE> report;
            if (WaitForGuideCid(*hidppDevice, basics, 5000, &cid, &report))
            {
                result.detected = true;
                result.cid = cid;
                result.report = report;
                std::wstringstream detected;
                detected << L"  已识别：CID" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << cid
                         << std::dec << std::setfill(L' ')
                         << L" data=" << BytesToHex(report.data(), static_cast<DWORD>(report.size()), 64);
                ConsoleWriteLine(detected.str());
                ConsoleWriteLine();
            }
            else
            {
                ConsoleWriteLine(L"  这一步没有识别到 HID++ CID");
                ConsoleWriteLine();
            }

            results.push_back(result);
            Sleep(300);
        }

        ConsoleWriteLine(L"正在恢复临时切换的按键上报模式。");
        HidppSession restoreSession(*hidppDevice);
        if (restoreSession.Open())
        {
            for (size_t i = 0; i < divertedCids.size(); ++i)
            {
                SetCidReporting(&restoreSession, basics, divertedCids[i], false, false);
            }
        }

        ConsoleWriteLine();
        ConsoleWriteLine(L"识别结果：");
        for (size_t i = 0; i < results.size(); ++i)
        {
            std::wstringstream line;
            line << L"  " << results[i].name << L" -> ";
            if (results[i].detected)
            {
                line << L"CID" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << results[i].cid
                     << std::dec << std::setfill(L' ');
            }
            else
            {
                line << L"未识别";
            }
            ConsoleWriteLine(line.str());
        }

        ConsoleWriteLine();
        ConsoleWriteLine(L"配置模板：");
        for (size_t i = 0; i < results.size(); ++i)
        {
            if (results[i].detected)
            {
                ConsoleWriteLine(std::wstring(L"# ") + results[i].name);
                std::wstringstream configLine;
                configLine << L"CID" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << results[i].cid
                           << std::dec << std::setfill(L' ')
                           << L" = Ctrl+Shift+P";
                ConsoleWriteLine(configLine.str());
            }
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    bool listOnly = false;
    bool hidppInfo = false;
    bool divertWatch = false;
    bool mxMaster4Guide = false;
    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--list") == 0)
        {
            listOnly = true;
        }
        else if (_wcsicmp(argv[i], L"--hidpp-info") == 0)
        {
            hidppInfo = true;
        }
        else if (_wcsicmp(argv[i], L"--divert-watch") == 0)
        {
            divertWatch = true;
        }
        else if (_wcsicmp(argv[i], L"--mx-master-4-guide") == 0 || _wcsicmp(argv[i], L"--guide") == 0)
        {
            mxMaster4Guide = true;
        }
    }

    std::wcout << L"MouseLogi HID Probe" << std::endl;
    std::wcout << L"Directly reads Logitech VID_046D HID input reports." << std::endl;
    if (!listOnly)
    {
        std::wcout << L"Press suspect mouse buttons one by one. Press Esc or Ctrl+C to exit." << std::endl;
    }
    std::wcout << std::endl;

    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_stopEvent == NULL)
    {
        std::wcerr << L"Failed to create stop event." << std::endl;
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::vector<HidDevice> devices = EnumerateLogitechHidDevices();
    PrintDeviceSummary(devices);
    std::wcout << std::endl;

    if (hidppInfo)
    {
        PrintHidppInfo(devices);
        CloseHandle(g_stopEvent);
        return 0;
    }

    if (divertWatch)
    {
        DivertAndWatch(devices);
        CloseHandle(g_stopEvent);
        return 0;
    }

    if (mxMaster4Guide)
    {
        RunMxMaster4Guide(devices);
        CloseHandle(g_stopEvent);
        return 0;
    }

    if (listOnly)
    {
        CloseHandle(g_stopEvent);
        return 0;
    }

    std::vector<std::thread> threads;
    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (devices[i].canOpenForRead && devices[i].inputReportLength > 0)
        {
            threads.push_back(std::thread(ReaderThread, devices[i]));
        }
    }

    if (threads.empty())
    {
        std::wcout << L"No readable Logitech HID input interfaces were found." << std::endl;
        std::wcout << L"Try closing Logi Options+ if it is running, then run this probe again." << std::endl;
    }
    else
    {
        std::wcout << L"Reading " << threads.size() << L" HID input interface(s)." << std::endl;
        std::wcout << L"Reports are printed only when bytes change." << std::endl;
        std::wcout << std::endl;
    }

    while (!g_stop.load())
    {
        if (_kbhit())
        {
            int key = _getch();
            if (key == 27)
            {
                g_stop.store(true);
                SetEvent(g_stopEvent);
                break;
            }
        }
        Sleep(50);
    }

    g_stop.store(true);
    SetEvent(g_stopEvent);

    for (size_t i = 0; i < threads.size(); ++i)
    {
        if (threads[i].joinable())
        {
            threads[i].join();
        }
    }

    CloseHandle(g_stopEvent);
    return 0;
}
