#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{
    enum Trigger
    {
        TriggerXButton1 = 0,
        TriggerXButton2,
        TriggerMButton,
        TriggerWheelUp,
        TriggerWheelDown,
        TriggerWheelLeft,
        TriggerWheelRight,
        TriggerCount
    };

    struct Shortcut
    {
        std::vector<WORD> modifiers;
        std::vector<WORD> keys;
        bool enabled = false;
    };

    HHOOK g_hook = NULL;
    Shortcut g_shortcuts[TriggerCount];
    std::map<USHORT, Shortcut> g_hidppShortcuts;
    const wchar_t kSingleInstanceMutexName[] = L"Local\\MouseLogiNative.SingleInstance";
    const wchar_t kQuitEventName[] = L"Local\\MouseLogiNative.QuitEvent";
    const wchar_t kTrayWindowClassName[] = L"MouseLogiNative.TrayWindow";
    const DWORD kResetWaitMs = 60000;
    const DWORD kResetPollMs = 1000;
    const UINT kTrayIconId = 1;
    const UINT kTrayCallbackMessage = WM_APP + 1;
    const UINT kTrayMenuOpenConfig = 1001;
    const UINT kTrayMenuReset = 1002;
    const UINT kTrayMenuExit = 1003;

    struct HidDevice
    {
        std::wstring path;
        std::string deviceId;
        USHORT vendorId = 0;
        USHORT productId = 0;
        USAGE usagePage = 0;
        USAGE usage = 0;
        USHORT inputReportLength = 0;
        USHORT outputReportLength = 0;
    };

    struct HidppBasics
    {
        BYTE deviceIndex = 0;
        BYTE featureSetIndex = 0;
        BYTE specialKeysIndex = 0;
    };

    HANDLE g_hidppHandle = INVALID_HANDLE_VALUE;
    HANDLE g_hidppStopEvent = NULL;
    HidDevice g_hidppDevice;
    HidppBasics g_hidppBasics;
    std::string g_activeMouseId;
    std::vector<USHORT> g_divertedCids;
    std::thread g_hidppThread;
    std::atomic<bool> g_hidppStop(false);
    std::wstring g_configPath;
    HWND g_trayWindow = NULL;
    HICON g_trayIcon = NULL;
    bool g_trayIconVisible = false;
    bool g_trayEffective = false;
    bool g_trayWarning = false;
    size_t g_trayMappingCount = 0;
    UINT g_taskbarCreatedMessage = 0;

    bool AddTrayIcon(HWND hwnd);
    void ShowTrayMenu(HWND hwnd);

    const char kDefaultConfig[] =
        "# MouseLogi Native config\r\n"
        "# Lines use: mouse_button = shortcut\r\n"
        "#\r\n"
        "# Buttons: XButton1, XButton2, MButton, WheelLeft, WheelRight, WheelUp, WheelDown\r\n"
        "# Logitech HID++ buttons: CID0053, CID0056, CID00C3, or aliases LogiBack, LogiForward, LogiGesture\r\n"
        "# Shortcuts: Ctrl+C, Ctrl+Shift+P, Alt+Left, RightAlt, RightAlt+E, Win+Shift+S\r\n"
        "# Device blocks are optional and use the Bluetooth device address from MouseLogiHidProbe.exe --list.\r\n"
        "# Example: [mouse:C7284491160D]\r\n"
        "\r\n"
        "XButton1 = Alt+Left\r\n"
        "XButton2 = Alt+Right\r\n"
        "\r\n"
        "# MButton = Ctrl+W\r\n"
        "# WheelLeft = Ctrl+PageUp\r\n"
        "# WheelRight = Ctrl+PageDown\r\n";

    bool HasCommandLineArgument(const wchar_t* expected)
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv == NULL)
        {
            return false;
        }

        bool found = false;
        for (int i = 1; i < argc; ++i)
        {
            if (lstrcmpiW(argv[i], expected) == 0)
            {
                found = true;
                break;
            }
        }

        LocalFree(argv);
        return found;
    }

    bool SignalQuitEvent()
    {
        HANDLE quitEvent = OpenEventW(EVENT_MODIFY_STATE, FALSE, kQuitEventName);
        if (quitEvent == NULL)
        {
            return false;
        }

        BOOL signaled = SetEvent(quitEvent);
        CloseHandle(quitEvent);
        return signaled != FALSE;
    }

    std::string Trim(const std::string& value)
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

    std::string NormalizeSectionName(const std::string& value)
    {
        std::string result;
        for (size_t i = 0; i < value.size(); ++i)
        {
            unsigned char ch = static_cast<unsigned char>(value[i]);
            if (std::isspace(ch) != 0 || ch == '-' || ch == '_')
            {
                continue;
            }
            result.push_back(static_cast<char>(std::toupper(ch)));
        }
        return result;
    }

    std::string NormalizeDeviceId(const std::string& value)
    {
        std::string upperValue(value);
        for (size_t i = 0; i < upperValue.size(); ++i)
        {
            upperValue[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(upperValue[i])));
        }

        size_t devIndex = upperValue.find("DEV");
        if (devIndex != std::string::npos)
        {
            std::string fromDev;
            for (size_t i = devIndex + 3; i < upperValue.size(); ++i)
            {
                unsigned char ch = static_cast<unsigned char>(upperValue[i]);
                if (std::isxdigit(ch) != 0)
                {
                    fromDev.push_back(static_cast<char>(ch));
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

        std::string result;
        for (size_t i = 0; i < upperValue.size(); ++i)
        {
            unsigned char ch = static_cast<unsigned char>(upperValue[i]);
            if (std::isxdigit(ch) == 0)
            {
                continue;
            }
            result.push_back(static_cast<char>(ch));
        }

        if (result.size() > 12)
        {
            result = result.substr(result.size() - 12);
        }
        return result;
    }

    bool IsGlobalSectionName(const std::string& name)
    {
        std::string normalized = NormalizeSectionName(name);
        return normalized == "GLOBAL" || normalized == "DEFAULT" || normalized == "ALL";
    }

    bool TryParseDeviceSectionId(const std::string& sectionName, std::string* deviceId)
    {
        std::string name = Trim(sectionName);
        if (name.empty() || IsGlobalSectionName(name))
        {
            return false;
        }

        std::string target = name;
        size_t separator = name.find_first_of(":=");
        if (separator != std::string::npos)
        {
            std::string prefix = NormalizeSectionName(name.substr(0, separator));
            if (prefix == "MOUSE" || prefix == "DEVICE" || prefix == "ID")
            {
                target = name.substr(separator + 1);
            }
        }
        else
        {
            size_t firstSpace = name.find_first_of(" \t");
            if (firstSpace != std::string::npos)
            {
                std::string prefix = NormalizeSectionName(name.substr(0, firstSpace));
                if (prefix == "MOUSE" || prefix == "DEVICE" || prefix == "ID")
                {
                    target = name.substr(firstSpace + 1);
                }
            }
        }

        std::string normalized = NormalizeDeviceId(target);
        if (normalized.size() < 6)
        {
            return false;
        }

        *deviceId = normalized;
        return true;
    }

    void AddUniqueString(std::vector<std::string>* values, const std::string& value)
    {
        if (std::find(values->begin(), values->end(), value) == values->end())
        {
            values->push_back(value);
        }
    }

    void StripUtf8Bom(std::string* content)
    {
        if (content->size() >= 3 &&
            static_cast<unsigned char>((*content)[0]) == 0xEF &&
            static_cast<unsigned char>((*content)[1]) == 0xBB &&
            static_cast<unsigned char>((*content)[2]) == 0xBF)
        {
            content->erase(0, 3);
        }
    }

    bool TryParseSectionHeader(const std::string& line, std::string* sectionName)
    {
        if (line.size() < 2 || line[0] != '[' || line[line.size() - 1] != ']')
        {
            return false;
        }

        *sectionName = Trim(line.substr(1, line.size() - 2));
        return true;
    }

    bool SectionApplies(const std::string& sectionName, const std::string& activeMouseId)
    {
        if (IsGlobalSectionName(sectionName))
        {
            return true;
        }

        std::string sectionDeviceId;
        return !activeMouseId.empty() &&
            TryParseDeviceSectionId(sectionName, &sectionDeviceId) &&
            sectionDeviceId == activeMouseId;
    }

    std::string NormalizeToken(const std::string& value)
    {
        std::string result;
        for (size_t i = 0; i < value.size(); ++i)
        {
            unsigned char ch = static_cast<unsigned char>(value[i]);
            if (std::isspace(ch) != 0 || ch == '-' || ch == '_')
            {
                continue;
            }
            result.push_back(static_cast<char>(std::toupper(ch)));
        }
        return result;
    }

    std::vector<std::string> SplitShortcut(const std::string& text)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= text.size())
        {
            size_t pos = text.find('+', start);
            if (pos == std::string::npos)
            {
                parts.push_back(Trim(text.substr(start)));
                break;
            }

            parts.push_back(Trim(text.substr(start, pos - start)));
            start = pos + 1;
        }
        return parts;
    }

    bool TryParseTrigger(const std::string& text, Trigger* trigger)
    {
        const std::string token = NormalizeToken(text);

        if (token == "X1" || token == "XBUTTON1" || token == "BACK" || token == "THUMBBACK")
        {
            *trigger = TriggerXButton1;
            return true;
        }
        if (token == "X2" || token == "XBUTTON2" || token == "FORWARD" || token == "THUMBFORWARD")
        {
            *trigger = TriggerXButton2;
            return true;
        }
        if (token == "MBUTTON" || token == "MIDDLE" || token == "MIDDLEBUTTON")
        {
            *trigger = TriggerMButton;
            return true;
        }
        if (token == "WHEELUP")
        {
            *trigger = TriggerWheelUp;
            return true;
        }
        if (token == "WHEELDOWN")
        {
            *trigger = TriggerWheelDown;
            return true;
        }
        if (token == "WHEELLEFT" || token == "HWHEELLEFT")
        {
            *trigger = TriggerWheelLeft;
            return true;
        }
        if (token == "WHEELRIGHT" || token == "HWHEELRIGHT")
        {
            *trigger = TriggerWheelRight;
            return true;
        }

        return false;
    }

    bool TryParseHexUshort(const std::string& token, USHORT* value)
    {
        if (token.empty() || token.size() > 4)
        {
            return false;
        }

        unsigned int parsed = 0;
        for (size_t i = 0; i < token.size(); ++i)
        {
            char ch = token[i];
            unsigned int nibble = 0;
            if (ch >= '0' && ch <= '9')
            {
                nibble = static_cast<unsigned int>(ch - '0');
            }
            else if (ch >= 'A' && ch <= 'F')
            {
                nibble = static_cast<unsigned int>(ch - 'A' + 10);
            }
            else
            {
                return false;
            }
            parsed = (parsed << 4) | nibble;
        }

        *value = static_cast<USHORT>(parsed);
        return true;
    }

    bool TryParseHidppCid(const std::string& text, USHORT* cid)
    {
        std::string token = NormalizeToken(text);

        if (token == "LOGIBACK")
        {
            *cid = 0x0053;
            return true;
        }
        if (token == "LOGIFORWARD")
        {
            *cid = 0x0056;
            return true;
        }
        if (token == "LOGIGESTURE" || token == "GESTURE")
        {
            *cid = 0x00C3;
            return true;
        }

        const char* prefixes[] = { "CID", "HID", "LOGI", "C" };
        for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i)
        {
            const std::string prefix(prefixes[i]);
            if (token.rfind(prefix, 0) == 0)
            {
                return TryParseHexUshort(token.substr(prefix.size()), cid);
            }
        }

        return false;
    }

    bool TryParseModifier(const std::string& token, WORD* key)
    {
        if (token == "CTRL" || token == "CONTROL")
        {
            *key = VK_CONTROL;
            return true;
        }
        if (token == "SHIFT")
        {
            *key = VK_SHIFT;
            return true;
        }
        if (token == "ALT" || token == "MENU")
        {
            *key = VK_MENU;
            return true;
        }
        if (token == "RIGHTALT" || token == "RALT" || token == "ALTGR")
        {
            *key = VK_RMENU;
            return true;
        }
        if (token == "LEFTALT" || token == "LALT")
        {
            *key = VK_LMENU;
            return true;
        }
        if (token == "WIN" || token == "WINDOWS" || token == "CMD" || token == "COMMAND")
        {
            *key = VK_LWIN;
            return true;
        }
        return false;
    }

    bool TryNamedKey(const std::string& token, WORD* key)
    {
        struct NamedKey
        {
            const char* name;
            WORD key;
        };

        static const NamedKey keys[] = {
            {"BACKSPACE", VK_BACK},
            {"BACK", VK_BACK},
            {"TAB", VK_TAB},
            {"ENTER", VK_RETURN},
            {"RETURN", VK_RETURN},
            {"ESC", VK_ESCAPE},
            {"ESCAPE", VK_ESCAPE},
            {"SPACE", VK_SPACE},
            {"PAGEUP", VK_PRIOR},
            {"PGUP", VK_PRIOR},
            {"PAGEDOWN", VK_NEXT},
            {"PGDN", VK_NEXT},
            {"END", VK_END},
            {"HOME", VK_HOME},
            {"LEFT", VK_LEFT},
            {"UP", VK_UP},
            {"RIGHT", VK_RIGHT},
            {"DOWN", VK_DOWN},
            {"INSERT", VK_INSERT},
            {"INS", VK_INSERT},
            {"DELETE", VK_DELETE},
            {"DEL", VK_DELETE},
            {"PLUS", VK_OEM_PLUS},
            {"MINUS", VK_OEM_MINUS},
            {"COMMA", VK_OEM_COMMA},
            {"PERIOD", VK_OEM_PERIOD},
            {"SLASH", VK_OEM_2},
            {"SEMICOLON", VK_OEM_1},
            {"QUOTE", VK_OEM_7},
            {"BACKTICK", VK_OEM_3},
            {"LBRACKET", VK_OEM_4},
            {"RBRACKET", VK_OEM_6},
            {"BACKSLASH", VK_OEM_5},
        };

        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        {
            if (token == keys[i].name)
            {
                *key = keys[i].key;
                return true;
            }
        }
        return false;
    }

    bool TryParseKey(const std::string& token, WORD* key)
    {
        if (token.size() == 1)
        {
            char ch = token[0];
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
            {
                *key = static_cast<WORD>(ch);
                return true;
            }
        }

        if (token.size() >= 2 && token[0] == 'F')
        {
            int value = 0;
            for (size_t i = 1; i < token.size(); ++i)
            {
                if (token[i] < '0' || token[i] > '9')
                {
                    value = 0;
                    break;
                }
                value = (value * 10) + (token[i] - '0');
            }

            if (value >= 1 && value <= 24)
            {
                *key = static_cast<WORD>(VK_F1 + value - 1);
                return true;
            }
        }

        return TryNamedKey(token, key);
    }

    void AddUnique(std::vector<WORD>* keys, WORD key)
    {
        if (std::find(keys->begin(), keys->end(), key) == keys->end())
        {
            keys->push_back(key);
        }
    }

    bool TryParseShortcut(const std::string& text, Shortcut* shortcut)
    {
        Shortcut parsed;
        std::vector<std::string> parts = SplitShortcut(text);

        for (size_t i = 0; i < parts.size(); ++i)
        {
            const std::string token = NormalizeToken(parts[i]);
            if (token.empty())
            {
                continue;
            }

            WORD key = 0;
            if (TryParseModifier(token, &key))
            {
                AddUnique(&parsed.modifiers, key);
                continue;
            }

            if (TryParseKey(token, &key))
            {
                parsed.keys.push_back(key);
                continue;
            }

            return false;
        }

        if (parsed.modifiers.empty() && parsed.keys.empty())
        {
            return false;
        }

        parsed.enabled = true;
        *shortcut = parsed;
        return true;
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

    std::wstring GetExePath()
    {
        wchar_t path[MAX_PATH] = {};
        DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (length == 0 || length == MAX_PATH)
        {
            return L"";
        }

        return std::wstring(path, length);
    }

    void AppendLog(const std::string& message)
    {
        std::wstring logPath = GetExeDirectory() + L"\\MouseLogiNative.log";
        HANDLE file = CreateFileW(
            logPath.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        SYSTEMTIME now = {};
        GetLocalTime(&now);

        char line[1200] = {};
        int length = _snprintf_s(
            line,
            sizeof(line),
            _TRUNCATE,
            "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu %s\r\n",
            static_cast<unsigned int>(now.wYear),
            static_cast<unsigned int>(now.wMonth),
            static_cast<unsigned int>(now.wDay),
            static_cast<unsigned int>(now.wHour),
            static_cast<unsigned int>(now.wMinute),
            static_cast<unsigned int>(now.wSecond),
            static_cast<unsigned int>(now.wMilliseconds),
            static_cast<unsigned long>(GetCurrentProcessId()),
            message.c_str());
        if (length < 0)
        {
            length = lstrlenA(line);
        }
        if (length > 0)
        {
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(length), &written, NULL);
        }

        CloseHandle(file);
    }

    uint32_t Argb(BYTE alpha, BYTE red, BYTE green, BYTE blue)
    {
        return (static_cast<uint32_t>(alpha) << 24) |
            (static_cast<uint32_t>(red) << 16) |
            (static_cast<uint32_t>(green) << 8) |
            static_cast<uint32_t>(blue);
    }

    void SetIconPixel(uint32_t* pixels, int size, int x, int y, uint32_t color)
    {
        if (x < 0 || y < 0 || x >= size || y >= size)
        {
            return;
        }

        pixels[y * size + x] = color;
    }

    void DrawIconLine(uint32_t* pixels, int size, int x1, int y1, int x2, int y2, int thickness, uint32_t color)
    {
        int dx = abs(x2 - x1);
        int sx = x1 < x2 ? 1 : -1;
        int dy = -abs(y2 - y1);
        int sy = y1 < y2 ? 1 : -1;
        int error = dx + dy;
        int radius = thickness / 2;

        for (;;)
        {
            for (int y = y1 - radius; y <= y1 + radius; ++y)
            {
                for (int x = x1 - radius; x <= x1 + radius; ++x)
                {
                    SetIconPixel(pixels, size, x, y, color);
                }
            }

            if (x1 == x2 && y1 == y2)
            {
                break;
            }

            int twiceError = 2 * error;
            if (twiceError >= dy)
            {
                error += dy;
                x1 += sx;
            }
            if (twiceError <= dx)
            {
                error += dx;
                y1 += sy;
            }
        }
    }

    HICON CreateTrayStatusIcon(bool effective, bool warning)
    {
        const int size = 32;
        HICON fallbackIcon = CopyIcon(LoadIconW(NULL, IDI_APPLICATION));
        BITMAPV5HEADER header = {};
        header.bV5Size = sizeof(header);
        header.bV5Width = size;
        header.bV5Height = -size;
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;

        HDC dc = GetDC(NULL);
        void* bits = NULL;
        HBITMAP colorBitmap = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, NULL, 0);
        ReleaseDC(NULL, dc);
        if (colorBitmap == NULL || bits == NULL)
        {
            return fallbackIcon;
        }

        uint32_t* pixels = static_cast<uint32_t*>(bits);
        for (int i = 0; i < size * size; ++i)
        {
            pixels[i] = 0;
        }

        BYTE red = effective ? 20 : (warning ? 220 : 115);
        BYTE green = effective ? 150 : (warning ? 145 : 115);
        BYTE blue = effective ? 80 : (warning ? 25 : 115);
        uint32_t fill = Argb(255, red, green, blue);
        uint32_t edge = Argb(255, 255, 255, 255);
        uint32_t dark = Argb(255, 35, 35, 35);

        int center = size / 2;
        int radius = 14;
        for (int y = 0; y < size; ++y)
        {
            for (int x = 0; x < size; ++x)
            {
                int dx = x - center;
                int dy = y - center;
                int distance = dx * dx + dy * dy;
                if (distance <= radius * radius)
                {
                    SetIconPixel(pixels, size, x, y, fill);
                }
            }
        }

        if (effective)
        {
            DrawIconLine(pixels, size, 8, 17, 13, 22, 4, edge);
            DrawIconLine(pixels, size, 13, 22, 24, 10, 4, edge);
        }
        else
        {
            uint32_t mark = warning ? dark : edge;
            DrawIconLine(pixels, size, 16, 9, 16, 20, 4, mark);
            DrawIconLine(pixels, size, 16, 24, 16, 24, 4, mark);
        }

        std::vector<BYTE> maskBits(size * 4, 0);
        HBITMAP maskBitmap = CreateBitmap(size, size, 1, 1, maskBits.data());
        if (maskBitmap == NULL)
        {
            DeleteObject(colorBitmap);
            return fallbackIcon;
        }

        ICONINFO iconInfo = {};
        iconInfo.fIcon = TRUE;
        iconInfo.hbmColor = colorBitmap;
        iconInfo.hbmMask = maskBitmap;

        HICON icon = CreateIconIndirect(&iconInfo);
        DeleteObject(colorBitmap);
        DeleteObject(maskBitmap);
        if (icon != NULL && fallbackIcon != NULL)
        {
            DestroyIcon(fallbackIcon);
        }
        return icon != NULL ? icon : fallbackIcon;
    }

    size_t CountStandardShortcuts()
    {
        size_t count = 0;
        for (int i = 0; i < TriggerCount; ++i)
        {
            if (g_shortcuts[i].enabled)
            {
                ++count;
            }
        }

        return count;
    }

    size_t CountHidppShortcuts()
    {
        size_t count = 0;
        for (std::map<USHORT, Shortcut>::const_iterator it = g_hidppShortcuts.begin(); it != g_hidppShortcuts.end(); ++it)
        {
            if (it->second.enabled)
            {
                ++count;
            }
        }

        return count;
    }

    std::wstring BuildTrayTooltip()
    {
        if (g_trayEffective)
        {
            return L"MouseLogi: shortcuts active (" + std::to_wstring(g_trayMappingCount) + L" mappings)";
        }

        if (g_trayWarning)
        {
            return L"MouseLogi: HID++ shortcuts unavailable";
        }

        return L"MouseLogi: no active shortcuts";
    }

    void FillTrayData(NOTIFYICONDATAW* data, HWND hwnd)
    {
        ZeroMemory(data, sizeof(*data));
        data->cbSize = sizeof(*data);
        data->hWnd = hwnd;
        data->uID = kTrayIconId;
    }

    bool AddTrayIcon(HWND hwnd)
    {
        if (hwnd == NULL)
        {
            return false;
        }

        NOTIFYICONDATAW data = {};
        FillTrayData(&data, hwnd);
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayCallbackMessage;
        data.hIcon = g_trayIcon;

        std::wstring tooltip = BuildTrayTooltip();
        lstrcpynW(data.szTip, tooltip.c_str(), ARRAYSIZE(data.szTip));

        BOOL ok = Shell_NotifyIconW(g_trayIconVisible ? NIM_MODIFY : NIM_ADD, &data);
        if (!ok && g_trayIconVisible)
        {
            ok = Shell_NotifyIconW(NIM_ADD, &data);
        }

        if (!ok)
        {
            AppendLog("Shell_NotifyIcon add failed");
            return false;
        }

        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        g_trayIconVisible = true;
        return true;
    }

    void RemoveTrayIcon()
    {
        if (!g_trayIconVisible || g_trayWindow == NULL)
        {
            return;
        }

        NOTIFYICONDATAW data = {};
        FillTrayData(&data, g_trayWindow);
        Shell_NotifyIconW(NIM_DELETE, &data);
        g_trayIconVisible = false;
    }

    void OpenConfigFile()
    {
        if (g_configPath.empty())
        {
            return;
        }

        HINSTANCE result = ShellExecuteW(NULL, L"open", g_configPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            AppendLog("open config failed");
        }
    }

    bool LaunchResetInstance()
    {
        std::wstring exePath = GetExePath();
        if (exePath.empty())
        {
            return false;
        }

        std::wstring commandLine = L"\"" + exePath + L"\" --reset";
        STARTUPINFOW startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION processInfo = {};

        BOOL ok = CreateProcessW(
            NULL,
            &commandLine[0],
            NULL,
            NULL,
            FALSE,
            0,
            NULL,
            NULL,
            &startup,
            &processInfo);
        if (ok)
        {
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
            return true;
        }

        AppendLog("launch reset failed; error=" + std::to_string(GetLastError()));
        return false;
    }

    void ShowTrayMenu(HWND hwnd)
    {
        POINT cursor = {};
        GetCursorPos(&cursor);

        HMENU menu = CreatePopupMenu();
        if (menu == NULL)
        {
            return;
        }

        AppendMenuW(menu, MF_STRING, kTrayMenuOpenConfig, L"Open config");
        AppendMenuW(menu, MF_STRING, kTrayMenuReset, L"Reload shortcuts");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"Exit");

        SetForegroundWindow(hwnd);
        UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, hwnd, NULL);
        DestroyMenu(menu);
        if (command != 0)
        {
            SendMessageW(hwnd, WM_COMMAND, command, 0);
        }
        PostMessageW(hwnd, WM_NULL, 0, 0);
    }

    LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage)
        {
            g_trayIconVisible = false;
            AddTrayIcon(hwnd);
            return 0;
        }

        switch (message)
        {
            case kTrayCallbackMessage:
                if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP)
                {
                    ShowTrayMenu(hwnd);
                }
                return 0;

            case WM_COMMAND:
                switch (LOWORD(wParam))
                {
                    case kTrayMenuOpenConfig:
                        OpenConfigFile();
                        return 0;
                    case kTrayMenuReset:
                        LaunchResetInstance();
                        return 0;
                    case kTrayMenuExit:
                        SignalQuitEvent();
                        return 0;
                    default:
                        break;
                }
                break;

            default:
                break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HWND CreateTrayWindow(HINSTANCE instance)
    {
        g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSEXW windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = TrayWindowProc;
        windowClass.hInstance = instance;
        windowClass.lpszClassName = kTrayWindowClassName;

        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            AppendLog("RegisterClassEx tray failed; error=" + std::to_string(GetLastError()));
            return NULL;
        }

        HWND hwnd = CreateWindowExW(
            0,
            kTrayWindowClassName,
            L"MouseLogiNative",
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            NULL,
            NULL,
            instance,
            NULL);
        if (hwnd == NULL)
        {
            AppendLog("CreateWindowEx tray failed; error=" + std::to_string(GetLastError()));
        }

        return hwnd;
    }

    bool StartTray(HINSTANCE instance, bool hidppStarted)
    {
        size_t standardCount = CountStandardShortcuts();
        size_t hidppCount = CountHidppShortcuts();
        bool hidppEffective = hidppCount == 0 || hidppStarted;

        g_trayMappingCount = standardCount + (hidppStarted ? hidppCount : 0);
        g_trayEffective = standardCount > 0 || (hidppStarted && hidppCount > 0);
        g_trayWarning = hidppCount > 0 && !hidppEffective;

        if (g_trayIcon != NULL)
        {
            DestroyIcon(g_trayIcon);
            g_trayIcon = NULL;
        }
        g_trayIcon = CreateTrayStatusIcon(g_trayEffective, g_trayWarning);

        g_trayWindow = CreateTrayWindow(instance);
        if (g_trayWindow == NULL)
        {
            return false;
        }

        return AddTrayIcon(g_trayWindow);
    }

    void StopTray()
    {
        RemoveTrayIcon();

        if (g_trayWindow != NULL)
        {
            DestroyWindow(g_trayWindow);
            g_trayWindow = NULL;
        }

        if (g_trayIcon != NULL)
        {
            DestroyIcon(g_trayIcon);
            g_trayIcon = NULL;
        }
    }

    bool ReadWholeFile(const std::wstring& path, std::string* content)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
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

    void ExtractConfiguredDeviceIds(const std::string& content, std::vector<std::string>* deviceIds)
    {
        size_t start = 0;
        while (start <= content.size())
        {
            size_t end = content.find_first_of("\r\n", start);
            std::string line;
            if (end == std::string::npos)
            {
                line = content.substr(start);
                start = content.size() + 1;
            }
            else
            {
                line = content.substr(start, end - start);
                start = end + 1;
                while (start < content.size() && (content[start] == '\r' || content[start] == '\n'))
                {
                    ++start;
                }
            }

            line = Trim(line);
            std::string sectionName;
            std::string deviceId;
            if (TryParseSectionHeader(line, &sectionName) &&
                TryParseDeviceSectionId(sectionName, &deviceId))
            {
                AddUniqueString(deviceIds, deviceId);
            }
        }
    }

    void EnsureDefaultConfig(const std::wstring& path)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            return;
        }

        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }

        DWORD written = 0;
        WriteFile(file, kDefaultConfig, static_cast<DWORD>(sizeof(kDefaultConfig) - 1), &written, NULL);
        CloseHandle(file);
    }

    void LoadConfig(const std::wstring& path, const std::string& activeMouseId)
    {
        for (int i = 0; i < TriggerCount; ++i)
        {
            g_shortcuts[i] = Shortcut();
        }
        g_hidppShortcuts.clear();

        EnsureDefaultConfig(path);

        std::string content;
        if (!ReadWholeFile(path, &content))
        {
            return;
        }

        StripUtf8Bom(&content);

        size_t start = 0;
        bool activeSection = true;
        while (start <= content.size())
        {
            size_t end = content.find_first_of("\r\n", start);
            std::string line;
            if (end == std::string::npos)
            {
                line = content.substr(start);
                start = content.size() + 1;
            }
            else
            {
                line = content.substr(start, end - start);
                start = end + 1;
                while (start < content.size() && (content[start] == '\r' || content[start] == '\n'))
                {
                    ++start;
                }
            }

            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
            {
                continue;
            }

            std::string sectionName;
            if (TryParseSectionHeader(line, &sectionName))
            {
                activeSection = SectionApplies(sectionName, activeMouseId);
                continue;
            }

            if (!activeSection)
            {
                continue;
            }

            size_t equals = line.find('=');
            if (equals == std::string::npos)
            {
                continue;
            }

            Trigger trigger;
            USHORT cid = 0;
            Shortcut shortcut;
            if (TryParseTrigger(line.substr(0, equals), &trigger) &&
                TryParseShortcut(line.substr(equals + 1), &shortcut))
            {
                g_shortcuts[trigger] = shortcut;
            }
            else if (TryParseHidppCid(line.substr(0, equals), &cid) &&
                     TryParseShortcut(line.substr(equals + 1), &shortcut))
            {
                g_hidppShortcuts[cid] = shortcut;
            }
        }
    }

    bool IsExtendedKey(WORD key)
    {
        switch (key)
        {
            case VK_PRIOR:
            case VK_NEXT:
            case VK_END:
            case VK_HOME:
            case VK_LEFT:
            case VK_UP:
            case VK_RIGHT:
            case VK_DOWN:
            case VK_INSERT:
            case VK_DELETE:
            case VK_LWIN:
            case VK_RWIN:
            case VK_RMENU:
            case VK_RCONTROL:
                return true;
            default:
                return false;
        }
    }

    void AddKeyInput(std::vector<INPUT>* inputs, WORD key, bool keyUp)
    {
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = key;
        input.ki.dwFlags = (keyUp ? KEYEVENTF_KEYUP : 0) | (IsExtendedKey(key) ? KEYEVENTF_EXTENDEDKEY : 0);
        inputs->push_back(input);
    }

    void SendShortcut(const Shortcut& shortcut)
    {
        std::vector<INPUT> inputs;
        inputs.reserve((shortcut.modifiers.size() + shortcut.keys.size()) * 2);

        for (size_t i = 0; i < shortcut.modifiers.size(); ++i)
        {
            AddKeyInput(&inputs, shortcut.modifiers[i], false);
        }
        for (size_t i = 0; i < shortcut.keys.size(); ++i)
        {
            AddKeyInput(&inputs, shortcut.keys[i], false);
        }
        for (size_t i = shortcut.keys.size(); i > 0; --i)
        {
            AddKeyInput(&inputs, shortcut.keys[i - 1], true);
        }
        for (size_t i = shortcut.modifiers.size(); i > 0; --i)
        {
            AddKeyInput(&inputs, shortcut.modifiers[i - 1], true);
        }

        if (!inputs.empty())
        {
            SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        }
    }

    std::string NarrowAscii(const std::wstring& value)
    {
        std::string result;
        for (size_t i = 0; i < value.size(); ++i)
        {
            wchar_t ch = value[i];
            if (ch <= 0x7F)
            {
                result.push_back(static_cast<char>(ch));
            }
        }
        return result;
    }

    std::wstring ToLowerAscii(const std::wstring& value)
    {
        std::wstring result(value);
        for (size_t i = 0; i < result.size(); ++i)
        {
            if (result[i] >= L'A' && result[i] <= L'Z')
            {
                result[i] = static_cast<wchar_t>(result[i] - L'A' + L'a');
            }
        }
        return result;
    }

    std::string ExtractDeviceIdFromPath(const std::wstring& path)
    {
        std::wstring lower = ToLowerAscii(path);
        size_t marker = lower.find(L"_dev_vid&");
        if (marker == std::wstring::npos)
        {
            return std::string();
        }

        size_t rev = lower.find(L"_rev&", marker);
        if (rev == std::wstring::npos)
        {
            return std::string();
        }

        size_t idStart = lower.find(L"_", rev + 5);
        if (idStart == std::wstring::npos || idStart + 1 >= lower.size())
        {
            return std::string();
        }
        ++idStart;

        size_t idEnd = lower.find_first_of(L"&#\\", idStart);
        std::wstring rawId = path.substr(idStart, idEnd == std::wstring::npos ? std::wstring::npos : idEnd - idStart);
        std::string deviceId = NormalizeDeviceId(NarrowAscii(rawId));
        if (deviceId.size() < 6)
        {
            return std::string();
        }
        return deviceId;
    }

    bool FillHidDeviceInfo(const std::wstring& path, HidDevice* device)
    {
        HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        HIDD_ATTRIBUTES attributes = {};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(handle, &attributes))
        {
            device->vendorId = attributes.VendorID;
            device->productId = attributes.ProductID;
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
            }
            HidD_FreePreparsedData(preparsedData);
        }

        CloseHandle(handle);
        return true;
    }

    bool FindHidppDevices(std::vector<HidDevice>* devices)
    {
        GUID hidGuid = {};
        HidD_GetHidGuid(&hidGuid);

        HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (deviceInfoSet == INVALID_HANDLE_VALUE)
        {
            return false;
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
            if (!FillHidDeviceInfo(device.path, &device))
            {
                continue;
            }

            if (device.vendorId == 0x046D &&
                device.usagePage == 0xFF43 &&
                device.usage == 0x0202 &&
                device.inputReportLength >= 20 &&
                device.outputReportLength >= 20)
            {
                device.deviceId = ExtractDeviceIdFromPath(device.path);
                devices->push_back(device);
            }
        }

        SetupDiDestroyDeviceInfoList(deviceInfoSet);
        return !devices->empty();
    }

    bool FindHidppDevice(HidDevice* foundDevice)
    {
        std::vector<HidDevice> devices;
        if (!FindHidppDevices(&devices))
        {
            return false;
        }

        *foundDevice = devices[0];
        return true;
    }

    void SelectActiveHidppDevice(const std::wstring& configPath)
    {
        g_activeMouseId.clear();
        g_hidppDevice = HidDevice();

        EnsureDefaultConfig(configPath);

        std::string content;
        std::vector<std::string> configuredDeviceIds;
        if (ReadWholeFile(configPath, &content))
        {
            StripUtf8Bom(&content);
            ExtractConfiguredDeviceIds(content, &configuredDeviceIds);
        }

        std::vector<HidDevice> devices;
        if (!FindHidppDevices(&devices))
        {
            return;
        }

        for (size_t idIndex = 0; idIndex < configuredDeviceIds.size(); ++idIndex)
        {
            for (size_t deviceIndex = 0; deviceIndex < devices.size(); ++deviceIndex)
            {
                if (devices[deviceIndex].deviceId == configuredDeviceIds[idIndex])
                {
                    g_hidppDevice = devices[deviceIndex];
                    g_activeMouseId = devices[deviceIndex].deviceId;
                    return;
                }
            }
        }

        g_hidppDevice = devices[0];
        g_activeMouseId = devices[0].deviceId;
    }

    void CancelPendingIo(HANDLE handle, OVERLAPPED* overlapped)
    {
        if (CancelIoEx(handle, overlapped) || GetLastError() != ERROR_NOT_FOUND)
        {
            DWORD transferred = 0;
            GetOverlappedResult(handle, overlapped, &transferred, TRUE);
        }
    }

    bool HidppWriteReport(const std::vector<BYTE>& report)
    {
        HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (event == NULL)
        {
            return false;
        }

        OVERLAPPED overlapped = {};
        overlapped.hEvent = event;
        DWORD written = 0;
        BOOL ok = WriteFile(g_hidppHandle, report.data(), static_cast<DWORD>(report.size()), &written, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            DWORD waitResult = WaitForSingleObject(event, 1000);
            if (waitResult == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(g_hidppHandle, &overlapped, &written, FALSE);
            }
            else
            {
                CancelPendingIo(g_hidppHandle, &overlapped);
                ok = FALSE;
            }
        }

        CloseHandle(event);
        return ok && written == report.size();
    }

    bool HidppReadReport(DWORD timeoutMs, std::vector<BYTE>* report)
    {
        report->assign(g_hidppDevice.inputReportLength, 0);

        HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (event == NULL)
        {
            return false;
        }

        OVERLAPPED overlapped = {};
        overlapped.hEvent = event;
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(g_hidppHandle, report->data(), static_cast<DWORD>(report->size()), &bytesRead, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            DWORD waitResult = WaitForSingleObject(event, timeoutMs);
            if (waitResult == WAIT_OBJECT_0)
            {
                ok = GetOverlappedResult(g_hidppHandle, &overlapped, &bytesRead, FALSE);
            }
            else
            {
                CancelPendingIo(g_hidppHandle, &overlapped);
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

    bool HidppRequest(BYTE deviceIndex, BYTE featureIndex, BYTE functionId, const std::vector<BYTE>& params, std::vector<BYTE>* response)
    {
        std::vector<BYTE> request(g_hidppDevice.outputReportLength, 0);
        request[0] = 0x11;
        request[1] = deviceIndex;
        request[2] = featureIndex;
        request[3] = static_cast<BYTE>((functionId << 4) | 0x0A);
        for (size_t i = 0; i < params.size() && 4 + i < request.size(); ++i)
        {
            request[4 + i] = params[i];
        }

        HidD_FlushQueue(g_hidppHandle);
        if (!HidppWriteReport(request))
        {
            return false;
        }

        DWORD deadline = GetTickCount() + 900;
        while (static_cast<LONG>(deadline - GetTickCount()) > 0)
        {
            std::vector<BYTE> candidate;
            DWORD timeout = deadline - GetTickCount();
            if (!HidppReadReport(timeout, &candidate))
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

    bool HidppIsError(const std::vector<BYTE>& response)
    {
        return response.size() >= 7 && response[2] == 0xFF;
    }

    bool HidppRequestParams(BYTE deviceIndex, BYTE featureIndex, BYTE functionId, const std::vector<BYTE>& params, std::vector<BYTE>* resultParams)
    {
        std::vector<BYTE> response;
        if (!HidppRequest(deviceIndex, featureIndex, functionId, params, &response) || HidppIsError(response) || response.size() <= 4)
        {
            return false;
        }

        resultParams->assign(response.begin() + 4, response.end());
        return true;
    }

    bool TryRootGetFeature(BYTE deviceIndex, USHORT featureId, BYTE* featureIndex)
    {
        std::vector<BYTE> params;
        params.push_back(static_cast<BYTE>((featureId >> 8) & 0xFF));
        params.push_back(static_cast<BYTE>(featureId & 0xFF));

        std::vector<BYTE> response;
        if (!HidppRequest(deviceIndex, 0x00, 0x00, params, &response) || HidppIsError(response) || response.size() < 5)
        {
            return false;
        }

        *featureIndex = response[4];
        return *featureIndex != 0;
    }

    bool InitializeHidppBasics()
    {
        const BYTE candidates[] = { 0xFF, 0x01, 0x00, 0x02, 0x03, 0x04, 0x05, 0x06 };
        for (size_t i = 0; i < sizeof(candidates); ++i)
        {
            BYTE featureSetIndex = 0;
            if (TryRootGetFeature(candidates[i], 0x0001, &featureSetIndex))
            {
                g_hidppBasics.deviceIndex = candidates[i];
                g_hidppBasics.featureSetIndex = featureSetIndex;
                break;
            }
        }

        if (g_hidppBasics.deviceIndex == 0 || g_hidppBasics.featureSetIndex == 0)
        {
            return false;
        }

        BYTE specialKeysIndex = 0;
        if (!TryRootGetFeature(g_hidppBasics.deviceIndex, 0x1B04, &specialKeysIndex))
        {
            return false;
        }

        g_hidppBasics.specialKeysIndex = specialKeysIndex;
        return true;
    }

    bool SetCidReporting(USHORT cid, bool divert)
    {
        BYTE reporting = 0x02; // dvalid
        if (divert)
        {
            reporting |= 0x01;
        }

        std::vector<BYTE> params;
        params.push_back(static_cast<BYTE>((cid >> 8) & 0xFF));
        params.push_back(static_cast<BYTE>(cid & 0xFF));
        params.push_back(reporting);
        params.push_back(0x00);
        params.push_back(0x00);

        std::vector<BYTE> result;
        return HidppRequestParams(g_hidppBasics.deviceIndex, g_hidppBasics.specialKeysIndex, 0x03, params, &result);
    }

    void HidppReaderThread()
    {
        std::vector<BYTE> buffer(g_hidppDevice.inputReportLength, 0);
        USHORT activeCid = 0;

        while (!g_hidppStop.load())
        {
            HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
            if (event == NULL)
            {
                return;
            }

            OVERLAPPED overlapped = {};
            overlapped.hEvent = event;
            DWORD bytesRead = 0;
            ResetEvent(event);

            BOOL ok = ReadFile(g_hidppHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, &overlapped);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
            {
                HANDLE waitHandles[2] = { g_hidppStopEvent, event };
                DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0)
                {
                    CancelPendingIo(g_hidppHandle, &overlapped);
                    CloseHandle(event);
                    break;
                }

                if (waitResult == WAIT_OBJECT_0 + 1)
                {
                    ok = GetOverlappedResult(g_hidppHandle, &overlapped, &bytesRead, FALSE);
                }
                else
                {
                    CloseHandle(event);
                    break;
                }
            }

            CloseHandle(event);
            if (!ok || bytesRead < 6)
            {
                continue;
            }

            if (buffer[0] != 0x11 ||
                buffer[1] != g_hidppBasics.deviceIndex ||
                buffer[2] != g_hidppBasics.specialKeysIndex ||
                buffer[3] != 0x00)
            {
                continue;
            }

            USHORT cid = static_cast<USHORT>((buffer[4] << 8) | buffer[5]);
            if (cid == 0)
            {
                activeCid = 0;
                continue;
            }

            if (cid == activeCid)
            {
                continue;
            }

            activeCid = cid;
            std::map<USHORT, Shortcut>::const_iterator found = g_hidppShortcuts.find(cid);
            if (found != g_hidppShortcuts.end() && found->second.enabled)
            {
                SendShortcut(found->second);
            }
        }
    }

    bool StartHidpp()
    {
        if (g_hidppShortcuts.empty())
        {
            return true;
        }

        if (g_hidppDevice.path.empty() && !FindHidppDevice(&g_hidppDevice))
        {
            return false;
        }

        g_hidppHandle = CreateFileW(
            g_hidppDevice.path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            NULL);
        if (g_hidppHandle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        if (!InitializeHidppBasics())
        {
            CloseHandle(g_hidppHandle);
            g_hidppHandle = INVALID_HANDLE_VALUE;
            return false;
        }

        for (std::map<USHORT, Shortcut>::const_iterator it = g_hidppShortcuts.begin(); it != g_hidppShortcuts.end(); ++it)
        {
            if (SetCidReporting(it->first, true))
            {
                g_divertedCids.push_back(it->first);
            }
        }

        if (g_divertedCids.empty())
        {
            CloseHandle(g_hidppHandle);
            g_hidppHandle = INVALID_HANDLE_VALUE;
            return false;
        }

        g_hidppStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (g_hidppStopEvent == NULL)
        {
            CloseHandle(g_hidppHandle);
            g_hidppHandle = INVALID_HANDLE_VALUE;
            return false;
        }

        g_hidppStop.store(false);
        g_hidppThread = std::thread(HidppReaderThread);
        return true;
    }

    void StopHidpp()
    {
        g_hidppStop.store(true);
        if (g_hidppStopEvent != NULL)
        {
            SetEvent(g_hidppStopEvent);
        }

        if (g_hidppThread.joinable())
        {
            g_hidppThread.join();
        }

        if (g_hidppHandle != INVALID_HANDLE_VALUE)
        {
            for (size_t i = 0; i < g_divertedCids.size(); ++i)
            {
                SetCidReporting(g_divertedCids[i], false);
            }

            CloseHandle(g_hidppHandle);
            g_hidppHandle = INVALID_HANDLE_VALUE;
        }

        if (g_hidppStopEvent != NULL)
        {
            CloseHandle(g_hidppStopEvent);
            g_hidppStopEvent = NULL;
        }
    }

    bool TryMessageToTrigger(WPARAM message, const MSLLHOOKSTRUCT* data, Trigger* trigger, bool* isRelease)
    {
        *isRelease = false;

        switch (message)
        {
            case WM_MBUTTONDOWN:
                *trigger = TriggerMButton;
                return true;
            case WM_MBUTTONUP:
                *trigger = TriggerMButton;
                *isRelease = true;
                return true;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            {
                WORD xButton = HIWORD(data->mouseData);
                if (xButton == XBUTTON1)
                {
                    *trigger = TriggerXButton1;
                }
                else if (xButton == XBUTTON2)
                {
                    *trigger = TriggerXButton2;
                }
                else
                {
                    return false;
                }

                *isRelease = (message == WM_XBUTTONUP);
                return true;
            }
            case WM_MOUSEWHEEL:
            {
                SHORT delta = static_cast<SHORT>(HIWORD(data->mouseData));
                *trigger = delta > 0 ? TriggerWheelUp : TriggerWheelDown;
                return true;
            }
            case WM_MOUSEHWHEEL:
            {
                SHORT delta = static_cast<SHORT>(HIWORD(data->mouseData));
                *trigger = delta > 0 ? TriggerWheelRight : TriggerWheelLeft;
                return true;
            }
            default:
                return false;
        }
    }

    LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode == HC_ACTION)
        {
            const MSLLHOOKSTRUCT* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
            Trigger trigger;
            bool isRelease = false;

            if (TryMessageToTrigger(wParam, data, &trigger, &isRelease) && g_shortcuts[trigger].enabled)
            {
                if (!isRelease)
                {
                    SendShortcut(g_shortcuts[trigger]);
                }
                return 1;
            }
        }

        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    bool WaitForResetMutex(HANDLE mutex)
    {
        DWORD waitedMs = 0;
        while (waitedMs < kResetWaitMs)
        {
            DWORD remainingMs = kResetWaitMs - waitedMs;
            DWORD waitMs = remainingMs < kResetPollMs ? remainingMs : kResetPollMs;
            DWORD waitResult = WaitForSingleObject(mutex, waitMs);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED)
            {
                return true;
            }

            if (waitResult != WAIT_TIMEOUT)
            {
                AppendLog("reset wait failed; error=" + std::to_string(GetLastError()));
                return false;
            }

            SignalQuitEvent();
            waitedMs += waitMs;
        }

        AppendLog("reset timed out waiting for previous instance");
        return false;
    }
}

int RunMouseLogiNative(HINSTANCE instance)
{
    bool quitRequested = HasCommandLineArgument(L"--quit") || HasCommandLineArgument(L"quit");
    bool resetRequested = HasCommandLineArgument(L"--reset") || HasCommandLineArgument(L"reset");

    if (quitRequested)
    {
        bool signaled = SignalQuitEvent();
        AppendLog(std::string("quit requested; signaled=") + (signaled ? "true" : "false"));
        return 0;
    }

    if (resetRequested)
    {
        bool signaled = SignalQuitEvent();
        AppendLog(std::string("reset requested; initial signal=") + (signaled ? "true" : "false"));
    }

    HANDLE mutex = CreateMutexW(NULL, TRUE, kSingleInstanceMutexName);
    if (mutex == NULL)
    {
        AppendLog("CreateMutex failed; error=" + std::to_string(GetLastError()));
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (!resetRequested)
        {
            AppendLog("instance already running; exiting duplicate");
            CloseHandle(mutex);
            return 0;
        }

        if (!WaitForResetMutex(mutex))
        {
            CloseHandle(mutex);
            return 1;
        }
    }

    std::wstring configPath = GetExeDirectory() + L"\\config.ini";
    g_configPath = configPath;
    SelectActiveHidppDevice(configPath);
    LoadConfig(configPath, g_activeMouseId);

    HANDLE quitEvent = CreateEventW(NULL, FALSE, FALSE, kQuitEventName);
    if (quitEvent == NULL)
    {
        AppendLog("CreateEvent failed; error=" + std::to_string(GetLastError()));
        CloseHandle(mutex);
        return 1;
    }

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, instance, 0);
    if (g_hook == NULL)
    {
        AppendLog("SetWindowsHookEx failed; error=" + std::to_string(GetLastError()));
        CloseHandle(quitEvent);
        CloseHandle(mutex);
        return 1;
    }

    bool hidppStarted = StartHidpp();
    bool trayStarted = StartTray(instance, hidppStarted);
    AppendLog(
        std::string("started; reset=") +
        (resetRequested ? "true" : "false") +
        "; activeMouseId=" + g_activeMouseId +
        "; hidppShortcuts=" + std::to_string(g_hidppShortcuts.size()) +
        "; hidppStarted=" + (hidppStarted ? "true" : "false") +
        "; trayStarted=" + (trayStarted ? "true" : "false") +
        "; trayEffective=" + (g_trayEffective ? "true" : "false"));

    MSG message;
    bool running = true;
    while (running)
    {
        DWORD waitResult = MsgWaitForMultipleObjects(1, &quitEvent, FALSE, INFINITE, QS_ALLINPUT);
        if (waitResult == WAIT_OBJECT_0)
        {
            break;
        }

        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    AppendLog("stopping");
    StopTray();
    StopHidpp();
    UnhookWindowsHookEx(g_hook);
    CloseHandle(quitEvent);
    CloseHandle(mutex);
    AppendLog("stopped");
    return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    try
    {
        return RunMouseLogiNative(instance);
    }
    catch (const std::exception& ex)
    {
        AppendLog(std::string("fatal exception: ") + ex.what());
        return 1;
    }
    catch (...)
    {
        AppendLog("fatal unknown exception");
        return 1;
    }
}
