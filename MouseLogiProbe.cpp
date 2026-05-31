#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

        std::wcout << L"Detected raw input devices:" << std::endl;
        for (UINT i = 0; i < count; ++i)
        {
            HANDLE device = devices[i].hDevice;
            int index = DeviceIndex(device);
            RID_DEVICE_INFO info = GetRawInputDeviceDetails(device);
            std::wstring name = GetRawInputDeviceName(device);

            std::wcout << L"  [" << index << L"] ";
            if (devices[i].dwType == RIM_TYPEMOUSE)
            {
                std::wcout << L"MOUSE buttons=" << info.mouse.dwNumberOfButtons
                           << L" sampleRate=" << info.mouse.dwSampleRate;
            }
            else if (devices[i].dwType == RIM_TYPEKEYBOARD)
            {
                std::wcout << L"KEYBOARD keys=" << info.keyboard.dwNumberOfKeysTotal;
            }
            else if (devices[i].dwType == RIM_TYPEHID)
            {
                std::wcout << L"HID page=" << HexWord(info.hid.usUsagePage)
                           << L" usage=" << HexWord(info.hid.usUsage);
            }
            else
            {
                std::wcout << L"UNKNOWN";
            }

            if (!name.empty())
            {
                std::wcout << L" " << name;
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
                return L"Left down";
            case WM_LBUTTONUP:
                return L"Left up";
            case WM_RBUTTONDOWN:
                return L"Right down";
            case WM_RBUTTONUP:
                return L"Right up";
            case WM_MBUTTONDOWN:
                return L"Middle down";
            case WM_MBUTTONUP:
                return L"Middle up";
            case WM_XBUTTONDOWN:
                return HIWORD(mouseData) == XBUTTON1 ? L"XButton1 down" : L"XButton2 down";
            case WM_XBUTTONUP:
                return HIWORD(mouseData) == XBUTTON1 ? L"XButton1 up" : L"XButton2 up";
            case WM_MOUSEWHEEL:
                return GET_WHEEL_DELTA_WPARAM(mouseData) > 0 ? L"Wheel up" : L"Wheel down";
            case WM_MOUSEHWHEEL:
                return GET_WHEEL_DELTA_WPARAM(mouseData) > 0 ? L"Horizontal wheel right" : L"Horizontal wheel left";
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
                std::wcout << L"HOOK mouse: " << name << std::endl;
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

        std::wcout << L"RAW  mouse [dev " << DeviceIndex(device) << L"]: ";

        if ((mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) std::wcout << L"Left down ";
        if ((mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) != 0) std::wcout << L"Left up ";
        if ((mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) std::wcout << L"Right down ";
        if ((mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) != 0) std::wcout << L"Right up ";
        if ((mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0) std::wcout << L"Middle down ";
        if ((mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) != 0) std::wcout << L"Middle up ";
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) != 0) std::wcout << L"Button4/XButton1 down ";
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP) != 0) std::wcout << L"Button4/XButton1 up ";
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) != 0) std::wcout << L"Button5/XButton2 down ";
        if ((mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP) != 0) std::wcout << L"Button5/XButton2 up ";
        if ((mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0)
        {
            SHORT delta = static_cast<SHORT>(mouse.usButtonData);
            std::wcout << (delta > 0 ? L"Wheel up " : L"Wheel down ") << L"delta=" << delta << L" ";
        }
        if ((mouse.usButtonFlags & RI_MOUSE_HWHEEL) != 0)
        {
            SHORT delta = static_cast<SHORT>(mouse.usButtonData);
            std::wcout << (delta > 0 ? L"HWheel right " : L"HWheel left ") << L"delta=" << delta << L" ";
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

        std::wcout << L"RAW  key   [dev " << DeviceIndex(device) << L"]: vk=" << HexWord(keyboard.VKey)
                   << L" scan=" << HexWord(keyboard.MakeCode)
                   << (isDown ? L" down" : L" up")
                   << std::endl;

        if (isDown && keyboard.VKey == VK_ESCAPE)
        {
            PostQuitMessage(0);
        }
    }

    void PrintRawHid(const RAWHID& hid, HANDLE device)
    {
        std::wcout << L"RAW  hid   [dev " << DeviceIndex(device) << L"]: size=" << hid.dwSizeHid
                   << L" count=" << hid.dwCount << L" data=";

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
            L"MouseLogi Probe",
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

        std::wcout << L"Registered raw input usages:" << std::endl;
        std::wcout << L"  page=" << HexWord(0x01) << L" usage=" << HexWord(0x02) << L" Mouse" << std::endl;
        std::wcout << L"  page=" << HexWord(0x01) << L" usage=" << HexWord(0x06) << L" Keyboard" << std::endl;
        for (size_t i = 0; i < hidUsages.size(); ++i)
        {
            std::wcout << L"  page=" << HexWord(hidUsages[i].usagePage)
                       << L" usage=" << HexWord(hidUsages[i].usage)
                       << L" HID" << std::endl;
        }
        std::wcout << std::endl;

        return RegisterRawInputDevices(devices.data(), static_cast<UINT>(devices.size()), sizeof(RAWINPUTDEVICE)) != FALSE;
    }
}

int wmain()
{
    HINSTANCE instance = GetModuleHandleW(NULL);

    std::wcout << L"MouseLogi Probe" << std::endl;
    std::wcout << L"Press mouse buttons one by one. Press Esc to exit." << std::endl;
    std::wcout << L"If a button prints nothing here, it is not exposed as standard mouse/keyboard/consumer input." << std::endl;
    std::wcout << std::endl;

    PrintDeviceList();

    if (!CreateHiddenWindow(instance))
    {
        std::wcerr << L"Failed to create hidden window." << std::endl;
        return 1;
    }

    if (!RegisterRawInputs())
    {
        std::wcerr << L"Failed to register raw input devices." << std::endl;
        return 1;
    }

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance, 0);
    if (g_mouseHook == NULL)
    {
        std::wcerr << L"Failed to install mouse hook." << std::endl;
        return 1;
    }

    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    UnhookWindowsHookEx(g_mouseHook);
    return 0;
}
