#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    HHOOK g_mouseHook = NULL;
    HWND g_window = NULL;
    std::map<HANDLE, int> g_deviceIndexes;
    std::wstring g_targetMouseId;
    std::map<std::wstring, std::wstring> g_mouseNamesById;
    std::map<std::wstring, std::set<std::wstring> > g_detectedTriggersById;

    struct HidUsage
    {
        USHORT usagePage;
        USHORT usage;

        bool operator<(const HidUsage& other) const
        {
            if (usagePage != other.usagePage)
            {
                return usagePage < other.usagePage;
            }
            return usage < other.usage;
        }
    };

    std::wstring HexPtr(HANDLE value)
    {
        std::wstringstream stream;
        stream << L"0x" << std::hex << reinterpret_cast<ULONG_PTR>(value);
        return stream.str();
    }

    std::wstring HexWord(USHORT value)
    {
        std::wstringstream stream;
        stream << L"0x" << std::uppercase << std::hex << std::setw(4) << std::setfill(L'0') << value;
        return stream.str();
    }

    std::wstring HexDword(ULONG value)
    {
        std::wstringstream stream;
        stream << L"0x" << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0') << value;
        return stream.str();
    }

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

    std::wstring NormalizeDeviceId(const std::wstring& value)
    {
        std::wstring upperValue(value);
        for (size_t i = 0; i < upperValue.size(); ++i)
        {
            if (upperValue[i] >= L'a' && upperValue[i] <= L'z')
            {
                upperValue[i] = static_cast<wchar_t>(upperValue[i] - L'a' + L'A');
            }
        }

        size_t devIndex = upperValue.find(L"DEV");
        if (devIndex != std::wstring::npos)
        {
            std::wstring fromDev;
            for (size_t i = devIndex + 3; i < upperValue.size(); ++i)
            {
                wchar_t ch = upperValue[i];
                bool hex = (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F');
                if (hex)
                {
                    fromDev.push_back(ch);
                    if (fromDev.size() == 12)
                    {
                        return fromDev;
                    }
                }
                else if (!fromDev.empty())
                {
                    fromDev.clear();
                }
            }
        }

        std::wstring result;
        for (size_t i = 0; i < upperValue.size(); ++i)
        {
            wchar_t ch = upperValue[i];
            if ((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'F'))
            {
                result.push_back(ch);
            }
        }
        if (result.size() > 12)
        {
            result = result.substr(result.size() - 12);
        }
        return result;
    }

    std::wstring ExtractDeviceIdFromPath(const std::wstring& path)
    {
        std::wstring lower = ToLower(path);
        size_t marker = lower.find(L"_dev_vid&");
        if (marker == std::wstring::npos)
        {
            marker = lower.find(L"#dev_vid&");
        }
        if (marker == std::wstring::npos)
        {
            return std::wstring();
        }

        size_t rev = lower.find(L"_rev&", marker);
        if (rev == std::wstring::npos)
        {
            return std::wstring();
        }

        size_t idStart = lower.find(L"_", rev + 5);
        if (idStart == std::wstring::npos || idStart + 1 >= lower.size())
        {
            return std::wstring();
        }
        ++idStart;

        size_t idEnd = lower.find_first_of(L"&#\\", idStart);
        std::wstring deviceId = NormalizeDeviceId(path.substr(idStart, idEnd == std::wstring::npos ? std::wstring::npos : idEnd - idStart));
        return deviceId.size() >= 6 ? deviceId : std::wstring();
    }

    bool TryParseHexWord(const std::wstring& text, USHORT* value)
    {
        if (text.size() != 4)
        {
            return false;
        }

        USHORT parsed = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            wchar_t ch = text[i];
            int digit = -1;
            if (ch >= L'0' && ch <= L'9')
            {
                digit = ch - L'0';
            }
            else if (ch >= L'a' && ch <= L'f')
            {
                digit = ch - L'a' + 10;
            }
            else if (ch >= L'A' && ch <= L'F')
            {
                digit = ch - L'A' + 10;
            }
            if (digit < 0)
            {
                return false;
            }
            parsed = static_cast<USHORT>((parsed << 4) | digit);
        }

        *value = parsed;
        return true;
    }

    USHORT ExtractProductIdFromPath(const std::wstring& path)
    {
        std::wstring lower = ToLower(path);
        const wchar_t* markers[] = { L"_pid&", L"pid&", L"pid_" };
        for (size_t markerIndex = 0; markerIndex < sizeof(markers) / sizeof(markers[0]); ++markerIndex)
        {
            size_t pid = lower.find(markers[markerIndex]);
            if (pid == std::wstring::npos)
            {
                continue;
            }
            size_t start = pid + std::wcslen(markers[markerIndex]);
            if (start + 4 <= path.size())
            {
                USHORT value = 0;
                if (TryParseHexWord(path.substr(start, 4), &value))
                {
                    return value;
                }
            }
        }
        return 0;
    }

    std::wstring KnownMouseName(USHORT productId)
    {
        switch (productId)
        {
            case 0xB023:
                return L"MX Master 3";
            case 0xB025:
                return L"MX Anywhere 3";
            case 0xB042:
                return L"MX Master 4";
            default:
                if (productId != 0)
                {
                    return L"Logitech PID " + HexWord(productId);
                }
                return L"未知设备";
        }
    }

    std::wstring NarrowAscii(const std::wstring& value)
    {
        std::wstring result;
        for (size_t i = 0; i < value.size(); ++i)
        {
            wchar_t ch = value[i];
            if (ch >= 0x20 && ch <= 0x7E)
            {
                result.push_back(ch);
            }
        }
        return result;
    }

    int DeviceIndex(HANDLE device)
    {
        std::map<HANDLE, int>::iterator found = g_deviceIndexes.find(device);
        if (found != g_deviceIndexes.end())
        {
            return found->second;
        }

        int index = static_cast<int>(g_deviceIndexes.size()) + 1;
        g_deviceIndexes[device] = index;
        return index;
    }

    std::wstring GetRawInputDeviceName(HANDLE device)
    {
        UINT size = 0;
        GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, NULL, &size);
        if (size == 0)
        {
            return L"";
        }

        std::vector<wchar_t> buffer(size + 1);
        if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, buffer.data(), &size) == static_cast<UINT>(-1))
        {
            return L"";
        }
        return std::wstring(buffer.data());
    }

    RID_DEVICE_INFO GetRawInputDeviceDetails(HANDLE device)
    {
        RID_DEVICE_INFO info = {};
        info.cbSize = sizeof(info);
        UINT size = sizeof(info);
        GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &info, &size);
        return info;
    }

    struct MouseIdentity
    {
        std::wstring deviceName;
        std::wstring deviceId;
        USHORT productId = 0;
        std::wstring rawName;
    };

    MouseIdentity GetMouseIdentity(HANDLE device)
    {
        MouseIdentity identity;
        identity.rawName = GetRawInputDeviceName(device);
        identity.deviceId = ExtractDeviceIdFromPath(identity.rawName);
        identity.productId = ExtractProductIdFromPath(identity.rawName);
        identity.deviceName = KnownMouseName(identity.productId);
        return identity;
    }

    void RecordTrigger(HANDLE device, const wchar_t* trigger)
    {
        MouseIdentity identity = GetMouseIdentity(device);
        if (identity.deviceId.empty())
        {
            return;
        }

        g_mouseNamesById[identity.deviceId] = identity.deviceName;
        g_detectedTriggersById[identity.deviceId].insert(trigger);
    }

    void PrintDeviceList()
    {
        UINT count = 0;
        if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        {
            return;
        }

        std::vector<RAWINPUTDEVICELIST> devices(count);
        if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
        {
            return;
        }

        std::wcout << L"检测到的 Raw Input 设备：" << std::endl;
        for (UINT i = 0; i < count; ++i)
        {
            HANDLE device = devices[i].hDevice;
            int index = DeviceIndex(device);
            RID_DEVICE_INFO info = GetRawInputDeviceDetails(device);
            MouseIdentity identity = GetMouseIdentity(device);

            std::wcout << L"  [" << index << L"] ";
            if (devices[i].dwType == RIM_TYPEMOUSE)
            {
                std::wcout << L"鼠标 按键数=" << info.mouse.dwNumberOfButtons
                           << L" 采样率=" << info.mouse.dwSampleRate;
            }
            else if (devices[i].dwType == RIM_TYPEKEYBOARD)
            {
                std::wcout << L"键盘 按键数=" << info.keyboard.dwNumberOfKeysTotal;
            }
            else if (devices[i].dwType == RIM_TYPEHID)
            {
                std::wcout << L"HID page=" << HexWord(info.hid.usUsagePage)
                           << L" usage=" << HexWord(info.hid.usUsage);
            }
            else
            {
                std::wcout << L"未知";
            }

            if (!identity.deviceName.empty())
            {
                std::wcout << L" name=\"" << identity.deviceName << L"\"";
            }
            if (!identity.deviceId.empty())
            {
                std::wcout << L" deviceId=" << identity.deviceId;
            }
            if (!identity.rawName.empty())
            {
                std::wcout << L" " << identity.rawName;
            }
            std::wcout << std::endl;
        }
        std::wcout << std::endl;
    }

    std::wstring MouseMessageName(WPARAM message, DWORD mouseData)
    {
        switch (message)
        {
            case WM_LBUTTONDOWN:
                return L"左键按下";
            case WM_LBUTTONUP:
                return L"左键抬起";
            case WM_RBUTTONDOWN:
                return L"右键按下";
            case WM_RBUTTONUP:
                return L"右键抬起";
            case WM_MBUTTONDOWN:
                return L"中键按下";
            case WM_MBUTTONUP:
                return L"中键抬起";
            case WM_XBUTTONDOWN:
                return HIWORD(mouseData) == XBUTTON1 ? L"XButton1 按下" : L"XButton2 按下";
            case WM_XBUTTONUP:
                return HIWORD(mouseData) == XBUTTON1 ? L"XButton1 抬起" : L"XButton2 抬起";
            case WM_MOUSEWHEEL:
                return GET_WHEEL_DELTA_WPARAM(mouseData) > 0 ? L"滚轮向上" : L"滚轮向下";
            case WM_MOUSEHWHEEL:
                return GET_WHEEL_DELTA_WPARAM(mouseData) > 0 ? L"横向滚轮向右" : L"横向滚轮向左";
            default:
                return L"";
        }
    }

    LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode == HC_ACTION)
        {
            const MSLLHOOKSTRUCT* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            std::wstring name = MouseMessageName(wParam, data->mouseData);
            if (!name.empty())
            {
                std::wcout << L"HOOK 鼠标：" << name << std::endl;
            }
        }

        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    void PrintRawMouse(const RAWMOUSE& mouse, HANDLE device)
    {
        if (mouse.usButtonFlags == 0 && mouse.ulRawButtons == 0)
        {
            return;
        }

        std::wcout << L"RAW  鼠标 [设备 " << DeviceIndex(device) << L"]：";

        if ((mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) std::wcout << L"左键按下 ";
        if ((mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) != 0) std::wcout << L"左键抬起 ";
        if ((mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) std::wcout << L"右键按下 ";
        if ((mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) != 0) std::wcout << L"右键抬起 ";
        if ((mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0)
        {
            std::wcout << L"中键按下 ";
            RecordTrigger(device, L"MButton");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) != 0) std::wcout << L"中键抬起 ";
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) != 0)
        {
            std::wcout << L"Button4/XButton1 按下 ";
            RecordTrigger(device, L"XButton1");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP) != 0) std::wcout << L"Button4/XButton1 抬起 ";
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) != 0)
        {
            std::wcout << L"Button5/XButton2 按下 ";
            RecordTrigger(device, L"XButton2");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP) != 0) std::wcout << L"Button5/XButton2 抬起 ";
        if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0)
        {
            SHORT delta = static_cast<SHORT>(mouse.usButtonData);
            std::wcout << (delta > 0 ? L"滚轮向上 " : L"滚轮向下 ") << L"delta=" << delta << L" ";
            RecordTrigger(device, delta > 0 ? L"WheelUp" : L"WheelDown");
        }
        if ((mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0)
        {
            SHORT delta = static_cast<SHORT>(mouse.usButtonData);
            std::wcout << (delta > 0 ? L"横向滚轮向右 " : L"横向滚轮向左 ") << L"delta=" << delta << L" ";
            RecordTrigger(device, delta > 0 ? L"WheelRight" : L"WheelLeft");
        }
        if (mouse.ulRawButtons != 0)
        {
            std::wcout << L"rawButtons=" << HexDword(mouse.ulRawButtons) << L" ";
        }

        std::wcout << std::endl;
    }

    void PrintRawKeyboard(const RAWKEYBOARD& keyboard, HANDLE device)
    {
        bool isDown = keyboard.Message == WM_KEYDOWN || keyboard.Message == WM_SYSKEYDOWN;
        bool isUp = keyboard.Message == WM_KEYUP || keyboard.Message == WM_SYSKEYUP;
        if (!isDown && !isUp)
        {
            return;
        }

        std::wcout << L"RAW  键盘 [设备 " << DeviceIndex(device) << L"]：vk=" << HexWord(keyboard.VKey)
                   << L" scan=" << HexWord(keyboard.MakeCode)
                   << (isDown ? L" 按下" : L" 抬起")
                   << std::endl;

        if (isDown && keyboard.VKey == VK_ESCAPE)
        {
            PostQuitMessage(0);
        }
    }

    void PrintRawHid(const RAWHID& hid, HANDLE device)
    {
        std::wcout << L"RAW  HID   [设备 " << DeviceIndex(device) << L"]：长度=" << hid.dwSizeHid
                   << L" 数量=" << hid.dwCount << L" 数据=";

        const BYTE* bytes = hid.bRawData;
        DWORD total = hid.dwSizeHid * hid.dwCount;
        DWORD limit = total < 32 ? total : 32;
        for (DWORD i = 0; i < limit; ++i)
        {
            std::wcout << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0')
                       << static_cast<unsigned int>(bytes[i]) << L" ";
        }
        std::wcout << std::dec;
        if (total > limit)
        {
            std::wcout << L"...";
        }
        std::wcout << std::endl;
    }

    void HandleRawInput(HRAWINPUT input)
    {
        UINT size = 0;
        GetRawInputData(input, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER));
        if (size == 0)
        {
            return;
        }

        std::vector<BYTE> buffer(size);
        if (GetRawInputData(input, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size)
        {
            return;
        }

        const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer.data());
        if (raw->header.dwType == RIM_TYPEMOUSE)
        {
            PrintRawMouse(raw->data.mouse, raw->header.hDevice);
        }
        else if (raw->header.dwType == RIM_TYPEKEYBOARD)
        {
            PrintRawKeyboard(raw->data.keyboard, raw->header.hDevice);
        }
        else if (raw->header.dwType == RIM_TYPEHID)
        {
            PrintRawHid(raw->data.hid, raw->header.hDevice);
        }
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_INPUT)
        {
            HandleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
            return 0;
        }
        if (message == WM_DESTROY)
        {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool CreateHiddenWindow(HINSTANCE instance)
    {
        const wchar_t className[] = L"MouseLogiProbeWindow";

        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = className;

        if (RegisterClassW(&windowClass) == 0)
        {
            return false;
        }

        g_window = CreateWindowExW(
            0,
            className,
            L"MouseLogi 标准输入探测工具",
            WS_OVERLAPPED,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            1,
            1,
            NULL,
            NULL,
            instance,
            NULL);

        return g_window != NULL;
    }

    std::vector<HidUsage> EnumerateHidUsages()
    {
        std::set<HidUsage> unique;

        UINT count = 0;
        if (GetRawInputDeviceList(NULL, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
        {
            return std::vector<HidUsage>();
        }

        std::vector<RAWINPUTDEVICELIST> devices(count);
        if (GetRawInputDeviceList(devices.data(), &count, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
        {
            return std::vector<HidUsage>();
        }

        for (UINT i = 0; i < count; ++i)
        {
            if (devices[i].dwType != RIM_TYPEHID)
            {
                continue;
            }

            RID_DEVICE_INFO info = GetRawInputDeviceDetails(devices[i].hDevice);
            if (info.dwType != RIM_TYPEHID)
            {
                continue;
            }

            HidUsage usage = {};
            usage.usagePage = info.hid.usUsagePage;
            usage.usage = info.hid.usUsage;
            if (usage.usagePage != 0 && usage.usage != 0)
            {
                unique.insert(usage);
            }
        }

        return std::vector<HidUsage>(unique.begin(), unique.end());
    }

    bool RegisterRawInputs()
    {
        std::vector<RAWINPUTDEVICE> devices;

        RAWINPUTDEVICE mouse = {};
        mouse.usUsagePage = 0x01;
        mouse.usUsage = 0x02; // Mouse
        mouse.dwFlags = RIDEV_INPUTSINK;
        mouse.hwndTarget = g_window;
        devices.push_back(mouse);

        RAWINPUTDEVICE keyboard = {};
        keyboard.usUsagePage = 0x01;
        keyboard.usUsage = 0x06; // Keyboard
        keyboard.dwFlags = RIDEV_INPUTSINK;
        keyboard.hwndTarget = g_window;
        devices.push_back(keyboard);

        std::vector<HidUsage> hidUsages = EnumerateHidUsages();
        for (size_t i = 0; i < hidUsages.size(); ++i)
        {
            RAWINPUTDEVICE hid = {};
            hid.usUsagePage = hidUsages[i].usagePage;
            hid.usUsage = hidUsages[i].usage;
            hid.dwFlags = RIDEV_INPUTSINK;
            hid.hwndTarget = g_window;
            devices.push_back(hid);
        }

        std::wcout << L"已注册的 Raw Input 用法：" << std::endl;
        std::wcout << L"  page=" << HexWord(0x01) << L" usage=" << HexWord(0x02) << L" 鼠标" << std::endl;
        std::wcout << L"  page=" << HexWord(0x01) << L" usage=" << HexWord(0x06) << L" 键盘" << std::endl;
        for (size_t i = 0; i < hidUsages.size(); ++i)
        {
            std::wcout << L"  page=" << HexWord(hidUsages[i].usagePage)
                       << L" usage=" << HexWord(hidUsages[i].usage)
                       << L" HID" << std::endl;
        }
        std::wcout << std::endl;

        return RegisterRawInputDevices(devices.data(), static_cast<UINT>(devices.size()), sizeof(RAWINPUTDEVICE)) != FALSE;
    }

    std::wstring GetExeDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (length == 0 || length == MAX_PATH)
        {
            return L".";
        }

        std::wstring dir(path, length);
        size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            return L".";
        }
        return dir.substr(0, slash);
    }

    bool ReadTextFile(const std::wstring& path, std::string* content)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            content->clear();
            return false;
        }

        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
        {
            CloseHandle(file);
            return false;
        }

        content->assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        BOOL ok = TRUE;
        if (!content->empty())
        {
            ok = ReadFile(file, &(*content)[0], static_cast<DWORD>(content->size()), &read, NULL);
        }
        CloseHandle(file);
        if (!ok)
        {
            return false;
        }
        content->resize(read);
        return true;
    }

    bool WriteTextFile(const std::wstring& path, const std::string& content)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        DWORD written = 0;
        BOOL ok = TRUE;
        if (!content.empty())
        {
            ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, NULL);
        }
        CloseHandle(file);
        return ok && written == content.size();
    }

    std::string NarrowAsciiString(const std::wstring& value)
    {
        std::string result;
        for (size_t i = 0; i < value.size(); ++i)
        {
            wchar_t ch = value[i];
            if (ch >= 0x20 && ch <= 0x7E)
            {
                result.push_back(static_cast<char>(ch));
            }
        }
        return result;
    }

    std::string Utf8String(const std::wstring& value)
    {
        if (value.empty())
        {
            return std::string();
        }

        int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), NULL, 0, NULL, NULL);
        if (size <= 0)
        {
            return NarrowAsciiString(value);
        }

        std::string result(static_cast<size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), &result[0], size, NULL, NULL);
        return result;
    }

    std::string TrimAscii(const std::string& value)
    {
        size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0)
        {
            ++start;
        }

        size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
        {
            --end;
        }
        return value.substr(start, end - start);
    }

    std::string NormalizeDeviceIdAscii(const std::string& value)
    {
        std::wstring wide(value.begin(), value.end());
        return NarrowAsciiString(NormalizeDeviceId(wide));
    }

    bool TryGetSectionDeviceId(const std::string& line, std::string* deviceId)
    {
        std::string trimmed = TrimAscii(line);
        if (trimmed.size() < 2 || trimmed[0] != '[' || trimmed[trimmed.size() - 1] != ']')
        {
            return false;
        }

        std::string section = TrimAscii(trimmed.substr(1, trimmed.size() - 2));
        size_t separator = section.find_first_of(":=");
        std::string value = separator == std::string::npos ? section : section.substr(separator + 1);
        std::string normalized = NormalizeDeviceIdAscii(value);
        if (normalized.size() < 6)
        {
            return false;
        }

        *deviceId = normalized;
        return true;
    }

    std::vector<std::string> SplitLines(const std::string& content)
    {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= content.size())
        {
            size_t end = content.find_first_of("\r\n", start);
            if (end == std::string::npos)
            {
                lines.push_back(content.substr(start));
                break;
            }

            lines.push_back(content.substr(start, end - start));
            start = end + 1;
            while (start < content.size() && (content[start] == '\r' || content[start] == '\n'))
            {
                ++start;
            }
        }
        return lines;
    }

    std::string JoinLines(const std::vector<std::string>& lines)
    {
        std::string content;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            content += lines[i];
            content += "\r\n";
        }
        return content;
    }

    void AppendLinesToMouseBlock(const std::wstring& configPath, const std::wstring& deviceId, const std::wstring& mouseName, const std::vector<std::string>& linesToAppend)
    {
        if (deviceId.empty() || linesToAppend.empty())
        {
            return;
        }

        std::string content;
        ReadTextFile(configPath, &content);
        std::vector<std::string> lines = SplitLines(content);
        std::string id = NarrowAsciiString(deviceId);
        std::string name = Utf8String(mouseName);

        size_t blockStart = lines.size();
        size_t blockEnd = lines.size();
        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::string sectionId;
            if (TryGetSectionDeviceId(lines[i], &sectionId))
            {
                if (sectionId == id)
                {
                    blockStart = i;
                    blockEnd = lines.size();
                    for (size_t j = i + 1; j < lines.size(); ++j)
                    {
                        std::string ignored;
                        std::string trimmed = TrimAscii(lines[j]);
                        if (trimmed.size() >= 2 && trimmed[0] == '[' && trimmed[trimmed.size() - 1] == ']')
                        {
                            blockEnd = j;
                            break;
                        }
                    }
                    break;
                }
            }
        }

        std::vector<std::string> insertLines;
        insertLines.push_back("# MouseLogiProbe 检测到的标准鼠标事件");
        if (!name.empty())
        {
            insertLines.push_back("# 鼠标：" + name);
        }
        for (size_t i = 0; i < linesToAppend.size(); ++i)
        {
            insertLines.push_back(linesToAppend[i]);
        }

        if (blockStart == lines.size())
        {
            if (!lines.empty() && !TrimAscii(lines.back()).empty())
            {
                lines.push_back("");
            }
            lines.push_back("[mouse:" + id + "]");
            if (!name.empty())
            {
                lines.push_back("# 鼠标：" + name);
            }
            blockEnd = lines.size();
        }

        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(blockEnd), insertLines.begin(), insertLines.end());
        WriteTextFile(configPath, JoinLines(lines));
    }

    void WriteDetectedConfigResults()
    {
        if (g_detectedTriggersById.empty())
        {
            return;
        }

        std::wstring configPath = GetExeDirectory() + L"\\config.ini";
        std::wcout << std::endl;
        std::wcout << L"正在把检测到的标准鼠标事件写入 " << configPath << std::endl;

        for (std::map<std::wstring, std::set<std::wstring> >::const_iterator it = g_detectedTriggersById.begin(); it != g_detectedTriggersById.end(); ++it)
        {
            std::vector<std::string> lines;
            for (std::set<std::wstring>::const_iterator trigger = it->second.begin(); trigger != it->second.end(); ++trigger)
            {
                lines.push_back("# " + NarrowAsciiString(*trigger) + " = Ctrl+Shift+P");
            }

            std::wstring mouseName = g_mouseNamesById[it->first];
            AppendLinesToMouseBlock(configPath, it->first, mouseName, lines);
            std::wcout << L"  [" << it->first << L"] " << mouseName << L"：" << it->second.size() << L" 个触发项" << std::endl;
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    bool listOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--list") == 0)
        {
            listOnly = true;
        }
        else if ((_wcsicmp(argv[i], L"--mouse") == 0 || _wcsicmp(argv[i], L"--device") == 0 || _wcsicmp(argv[i], L"--id") == 0) && i + 1 < argc)
        {
            g_targetMouseId = NormalizeDeviceId(argv[++i]);
        }
    }

    HINSTANCE instance = GetModuleHandleW(NULL);

    std::wcout << L"MouseLogi 标准输入探测工具" << std::endl;
    std::wcout << L"请逐个按鼠标按键。按 Esc 退出。" << std::endl;
    std::wcout << L"如果某个按键没有任何输出，说明它没有暴露为标准鼠标、键盘或 Consumer Input 事件。" << std::endl;
    if (!g_targetMouseId.empty())
    {
        std::wcout << L"目标鼠标 ID：" << g_targetMouseId << std::endl;
    }
    std::wcout << std::endl;

    PrintDeviceList();
    if (listOnly)
    {
        return 0;
    }

    if (!CreateHiddenWindow(instance))
    {
        std::wcerr << L"创建隐藏窗口失败。" << std::endl;
        return 1;
    }

    if (!RegisterRawInputs())
    {
        std::wcerr << L"注册 Raw Input 设备失败。" << std::endl;
        return 1;
    }

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance, 0);
    if (g_mouseHook == NULL)
    {
        std::wcerr << L"安装鼠标 Hook 失败。" << std::endl;
        return 1;
    }

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnhookWindowsHookEx(g_mouseHook);
    WriteDetectedConfigResults();
    return 0;
}
