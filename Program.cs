using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

namespace MouseLogi
{
    internal static class Program
    {
        private static Mutex singleInstanceMutex;

        [STAThread]
        private static void Main()
        {
            bool created;
            singleInstanceMutex = new Mutex(true, "MouseLogi.SingleInstance", out created);
            if (!created)
            {
                return;
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new TrayAppContext());
            GC.KeepAlive(singleInstanceMutex);
        }
    }

    internal sealed class TrayAppContext : ApplicationContext
    {
        private readonly NotifyIcon notifyIcon;
        private readonly MouseHook mouseHook;
        private readonly string configPath;
        private Dictionary<string, Shortcut> mappings = new Dictionary<string, Shortcut>(StringComparer.OrdinalIgnoreCase);

        public TrayAppContext()
        {
            configPath = ConfigFile.Resolve();
            LoadConfig(true);

            notifyIcon = new NotifyIcon();
            notifyIcon.Icon = SystemIcons.Application;
            notifyIcon.Text = "MouseLogi";
            notifyIcon.Visible = true;
            notifyIcon.ContextMenuStrip = BuildMenu();

            mouseHook = new MouseHook(HandleMouseTrigger);
            try
            {
                mouseHook.Start();
            }
            catch (Exception ex)
            {
                MessageBox.Show("Failed to start mouse hook:\r\n" + ex.Message, "MouseLogi",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                ExitThread();
                return;
            }

            notifyIcon.ShowBalloonTip(1500, "MouseLogi", "Mouse shortcut mapper is running.", ToolTipIcon.Info);
        }

        private ContextMenuStrip BuildMenu()
        {
            ContextMenuStrip menu = new ContextMenuStrip();

            ToolStripMenuItem reload = new ToolStripMenuItem("Reload Config");
            reload.Click += delegate { LoadConfig(false); };

            ToolStripMenuItem openConfig = new ToolStripMenuItem("Open Config");
            openConfig.Click += delegate { OpenConfig(); };

            ToolStripMenuItem exit = new ToolStripMenuItem("Exit");
            exit.Click += delegate { ExitThread(); };

            menu.Items.Add(reload);
            menu.Items.Add(openConfig);
            menu.Items.Add(new ToolStripSeparator());
            menu.Items.Add(exit);
            return menu;
        }

        private bool HandleMouseTrigger(string trigger, bool isRelease)
        {
            Shortcut shortcut;
            if (!mappings.TryGetValue(trigger, out shortcut))
            {
                return false;
            }

            if (!isRelease)
            {
                ShortcutSender.Send(shortcut);
            }

            return true;
        }

        private void LoadConfig(bool quiet)
        {
            ConfigLoadResult result = ConfigFile.Load(configPath);
            mappings = result.Mappings;

            if (!quiet)
            {
                string message = "Loaded " + mappings.Count + " mapping(s).";
                if (result.Errors.Count > 0)
                {
                    message += " " + result.Errors.Count + " line(s) skipped.";
                }

                notifyIcon.ShowBalloonTip(2000, "MouseLogi", message, result.Errors.Count == 0 ? ToolTipIcon.Info : ToolTipIcon.Warning);
            }
        }

        private void OpenConfig()
        {
            try
            {
                ProcessStartInfo info = new ProcessStartInfo(configPath);
                info.UseShellExecute = true;
                Process.Start(info);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Failed to open config:\r\n" + ex.Message, "MouseLogi",
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        protected override void ExitThreadCore()
        {
            if (mouseHook != null)
            {
                mouseHook.Dispose();
            }

            if (notifyIcon != null)
            {
                notifyIcon.Visible = false;
                notifyIcon.Dispose();
            }

            base.ExitThreadCore();
        }
    }

    internal static class ConfigFile
    {
        private const string DefaultConfig =
@"# MouseLogi config
# Lines use: mouse_button = shortcut
#
# Common mouse buttons:
#   XButton1      rear side button / Back button
#   XButton2      front side button / Forward button
#   MButton       middle button
#   WheelLeft     horizontal wheel left
#   WheelRight    horizontal wheel right
#
# Common shortcut examples:
#   Ctrl+C
#   Ctrl+Shift+P
#   Alt+Left
#   RightAlt
#   RightAlt+E
#   Win+Shift+S

XButton1 = Alt+Left
XButton2 = Alt+Right

# Uncomment and change these if needed:
# MButton = Ctrl+W
# WheelLeft = Ctrl+PageUp
# WheelRight = Ctrl+PageDown
";

        public static string Resolve()
        {
            string localPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "config.ini");
            if (File.Exists(localPath))
            {
                return localPath;
            }

            try
            {
                File.WriteAllText(localPath, DefaultConfig, Encoding.UTF8);
                return localPath;
            }
            catch
            {
                string appDataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MouseLogi");
                Directory.CreateDirectory(appDataDir);
                string appDataPath = Path.Combine(appDataDir, "config.ini");
                if (!File.Exists(appDataPath))
                {
                    File.WriteAllText(appDataPath, DefaultConfig, Encoding.UTF8);
                }
                return appDataPath;
            }
        }

        public static ConfigLoadResult Load(string path)
        {
            ConfigLoadResult result = new ConfigLoadResult();
            if (!File.Exists(path))
            {
                File.WriteAllText(path, DefaultConfig, Encoding.UTF8);
            }

            string[] lines = File.ReadAllLines(path, Encoding.UTF8);
            bool activeSection = true;
            for (int i = 0; i < lines.Length; i++)
            {
                string raw = lines[i];
                string line = raw.Trim();
                if (line.Length == 0 || line.StartsWith("#") || line.StartsWith(";"))
                {
                    continue;
                }

                string sectionName;
                if (TryParseSectionHeader(line, out sectionName))
                {
                    activeSection = IsGlobalSection(sectionName);
                    continue;
                }

                if (!activeSection)
                {
                    continue;
                }

                int equalsIndex = line.IndexOf('=');
                if (equalsIndex < 1)
                {
                    result.Errors.Add("Line " + (i + 1) + ": missing '='.");
                    continue;
                }

                string triggerText = line.Substring(0, equalsIndex).Trim();
                string shortcutText = line.Substring(equalsIndex + 1).Trim();

                string trigger;
                if (!MouseHook.TryNormalizeTrigger(triggerText, out trigger))
                {
                    result.Errors.Add("Line " + (i + 1) + ": unknown mouse button '" + triggerText + "'.");
                    continue;
                }

                Shortcut shortcut;
                string error;
                if (!ShortcutParser.TryParse(shortcutText, out shortcut, out error))
                {
                    result.Errors.Add("Line " + (i + 1) + ": " + error);
                    continue;
                }

                result.Mappings[trigger] = shortcut;
            }

            return result;
        }

        private static bool TryParseSectionHeader(string line, out string sectionName)
        {
            if (line.Length >= 2 && line[0] == '[' && line[line.Length - 1] == ']')
            {
                sectionName = line.Substring(1, line.Length - 2).Trim();
                return true;
            }

            sectionName = null;
            return false;
        }

        private static bool IsGlobalSection(string sectionName)
        {
            string normalized = sectionName.Replace(" ", "").Replace("-", "").Replace("_", "").ToUpperInvariant();
            return normalized == "GLOBAL" || normalized == "DEFAULT" || normalized == "ALL";
        }
    }

    internal sealed class ConfigLoadResult
    {
        public readonly Dictionary<string, Shortcut> Mappings = new Dictionary<string, Shortcut>(StringComparer.OrdinalIgnoreCase);
        public readonly List<string> Errors = new List<string>();
    }

    internal sealed class MouseHook : IDisposable
    {
        private const int WH_MOUSE_LL = 14;
        private const int WM_MBUTTONDOWN = 0x0207;
        private const int WM_MBUTTONUP = 0x0208;
        private const int WM_MOUSEWHEEL = 0x020A;
        private const int WM_XBUTTONDOWN = 0x020B;
        private const int WM_XBUTTONUP = 0x020C;
        private const int WM_MOUSEHWHEEL = 0x020E;

        private readonly TriggerHandler handler;
        private readonly LowLevelMouseProc hookProc;
        private IntPtr hookId = IntPtr.Zero;

        public delegate bool TriggerHandler(string trigger, bool isRelease);

        public MouseHook(TriggerHandler handler)
        {
            this.handler = handler;
            hookProc = HookCallback;
        }

        public static bool TryNormalizeTrigger(string text, out string trigger)
        {
            string key = text.Trim().Replace(" ", "").Replace("-", "").Replace("_", "").ToUpperInvariant();
            switch (key)
            {
                case "X1":
                case "XBUTTON1":
                case "BACK":
                case "THUMBBACK":
                    trigger = "XButton1";
                    return true;
                case "X2":
                case "XBUTTON2":
                case "FORWARD":
                case "THUMBFORWARD":
                    trigger = "XButton2";
                    return true;
                case "MBUTTON":
                case "MIDDLE":
                case "MIDDLEBUTTON":
                    trigger = "MButton";
                    return true;
                case "WHEELUP":
                    trigger = "WheelUp";
                    return true;
                case "WHEELDOWN":
                    trigger = "WheelDown";
                    return true;
                case "WHEELLEFT":
                case "HWHEELLEFT":
                    trigger = "WheelLeft";
                    return true;
                case "WHEELRIGHT":
                case "HWHEELRIGHT":
                    trigger = "WheelRight";
                    return true;
                default:
                    trigger = null;
                    return false;
            }
        }

        public void Start()
        {
            if (hookId != IntPtr.Zero)
            {
                return;
            }

            using (Process currentProcess = Process.GetCurrentProcess())
            using (ProcessModule currentModule = currentProcess.MainModule)
            {
                hookId = SetWindowsHookEx(WH_MOUSE_LL, hookProc, GetModuleHandle(currentModule.ModuleName), 0);
            }

            if (hookId == IntPtr.Zero)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
        }

        public void Dispose()
        {
            if (hookId != IntPtr.Zero)
            {
                UnhookWindowsHookEx(hookId);
                hookId = IntPtr.Zero;
            }
        }

        private IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
        {
            if (nCode >= 0)
            {
                MSLLHOOKSTRUCT hookStruct = (MSLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(MSLLHOOKSTRUCT));
                string trigger;
                bool isRelease;
                bool sendOnlyOnPress;

                if (TryGetTrigger(wParam.ToInt32(), hookStruct.mouseData, out trigger, out isRelease, out sendOnlyOnPress))
                {
                    bool handled = handler(trigger, isRelease);
                    if (handled)
                    {
                        return (IntPtr)1;
                    }
                }
            }

            return CallNextHookEx(hookId, nCode, wParam, lParam);
        }

        private static bool TryGetTrigger(int message, uint mouseData, out string trigger, out bool isRelease, out bool sendOnlyOnPress)
        {
            trigger = null;
            isRelease = false;
            sendOnlyOnPress = true;

            if (message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)
            {
                trigger = "MButton";
                isRelease = message == WM_MBUTTONUP;
                return true;
            }

            if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP)
            {
                uint xButton = (mouseData >> 16) & 0xffff;
                if (xButton == 1)
                {
                    trigger = "XButton1";
                }
                else if (xButton == 2)
                {
                    trigger = "XButton2";
                }

                isRelease = message == WM_XBUTTONUP;
                return trigger != null;
            }

            if (message == WM_MOUSEWHEEL)
            {
                short delta = unchecked((short)((mouseData >> 16) & 0xffff));
                trigger = delta > 0 ? "WheelUp" : "WheelDown";
                return true;
            }

            if (message == WM_MOUSEHWHEEL)
            {
                short delta = unchecked((short)((mouseData >> 16) & 0xffff));
                trigger = delta > 0 ? "WheelRight" : "WheelLeft";
                return true;
            }

            return false;
        }

        private delegate IntPtr LowLevelMouseProc(int nCode, IntPtr wParam, IntPtr lParam);

        [StructLayout(LayoutKind.Sequential)]
        private struct POINT
        {
            public int x;
            public int y;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct MSLLHOOKSTRUCT
        {
            public POINT pt;
            public uint mouseData;
            public uint flags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr SetWindowsHookEx(int idHook, LowLevelMouseProc lpfn, IntPtr hMod, uint dwThreadId);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool UnhookWindowsHookEx(IntPtr hhk);

        [DllImport("user32.dll")]
        private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

        [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        private static extern IntPtr GetModuleHandle(string lpModuleName);
    }

    internal sealed class Shortcut
    {
        public readonly ushort[] Modifiers;
        public readonly ushort[] Keys;
        public readonly string DisplayText;

        public Shortcut(List<ushort> modifiers, List<ushort> keys, string displayText)
        {
            Modifiers = modifiers.ToArray();
            Keys = keys.ToArray();
            DisplayText = displayText;
        }
    }

    internal static class ShortcutParser
    {
        private const ushort VK_CONTROL = 0x11;
        private const ushort VK_SHIFT = 0x10;
        private const ushort VK_MENU = 0x12;
        private const ushort VK_LMENU = 0xA4;
        private const ushort VK_RMENU = 0xA5;
        private const ushort VK_LWIN = 0x5B;

        private static readonly Dictionary<string, ushort> NamedKeys = BuildNamedKeys();

        public static bool TryParse(string text, out Shortcut shortcut, out string error)
        {
            shortcut = null;
            error = null;

            if (string.IsNullOrWhiteSpace(text))
            {
                error = "empty shortcut.";
                return false;
            }

            List<ushort> modifiers = new List<ushort>();
            List<ushort> keys = new List<ushort>();
            string[] parts = text.Split(new char[] { '+' }, StringSplitOptions.RemoveEmptyEntries);

            for (int i = 0; i < parts.Length; i++)
            {
                string token = parts[i].Trim();
                if (token.Length == 0)
                {
                    continue;
                }

                ushort keyCode;
                string normalized = token.Replace(" ", "").Replace("-", "").Replace("_", "").ToUpperInvariant();
                if (TryParseModifier(normalized, out keyCode))
                {
                    AddUnique(modifiers, keyCode);
                }
                else if (TryParseKey(normalized, out keyCode))
                {
                    keys.Add(keyCode);
                }
                else
                {
                    error = "unknown key '" + token + "'.";
                    return false;
                }
            }

            if (modifiers.Count == 0 && keys.Count == 0)
            {
                error = "empty shortcut.";
                return false;
            }

            shortcut = new Shortcut(modifiers, keys, text);
            return true;
        }

        private static bool TryParseModifier(string token, out ushort keyCode)
        {
            switch (token)
            {
                case "CTRL":
                case "CONTROL":
                    keyCode = VK_CONTROL;
                    return true;
                case "SHIFT":
                    keyCode = VK_SHIFT;
                    return true;
                case "ALT":
                case "MENU":
                    keyCode = VK_MENU;
                    return true;
                case "RIGHTALT":
                case "RALT":
                case "ALTGR":
                    keyCode = VK_RMENU;
                    return true;
                case "LEFTALT":
                case "LALT":
                    keyCode = VK_LMENU;
                    return true;
                case "WIN":
                case "WINDOWS":
                case "CMD":
                case "COMMAND":
                    keyCode = VK_LWIN;
                    return true;
                default:
                    keyCode = 0;
                    return false;
            }
        }

        private static bool TryParseKey(string token, out ushort keyCode)
        {
            if (token.Length == 1)
            {
                char ch = token[0];
                if (ch >= 'A' && ch <= 'Z')
                {
                    keyCode = (ushort)ch;
                    return true;
                }

                if (ch >= '0' && ch <= '9')
                {
                    keyCode = (ushort)ch;
                    return true;
                }
            }

            if (token.Length >= 2 && token[0] == 'F')
            {
                int fNumber;
                if (int.TryParse(token.Substring(1), out fNumber) && fNumber >= 1 && fNumber <= 24)
                {
                    keyCode = (ushort)(0x70 + fNumber - 1);
                    return true;
                }
            }

            if (NamedKeys.TryGetValue(token, out keyCode))
            {
                return true;
            }

            keyCode = 0;
            return false;
        }

        private static void AddUnique(List<ushort> list, ushort value)
        {
            if (!list.Contains(value))
            {
                list.Add(value);
            }
        }

        private static Dictionary<string, ushort> BuildNamedKeys()
        {
            Dictionary<string, ushort> keys = new Dictionary<string, ushort>(StringComparer.OrdinalIgnoreCase);
            keys["BACKSPACE"] = 0x08;
            keys["BACK"] = 0x08;
            keys["TAB"] = 0x09;
            keys["ENTER"] = 0x0D;
            keys["RETURN"] = 0x0D;
            keys["ESC"] = 0x1B;
            keys["ESCAPE"] = 0x1B;
            keys["SPACE"] = 0x20;
            keys["PAGEUP"] = 0x21;
            keys["PGUP"] = 0x21;
            keys["PAGEDOWN"] = 0x22;
            keys["PGDN"] = 0x22;
            keys["END"] = 0x23;
            keys["HOME"] = 0x24;
            keys["LEFT"] = 0x25;
            keys["UP"] = 0x26;
            keys["RIGHT"] = 0x27;
            keys["DOWN"] = 0x28;
            keys["INSERT"] = 0x2D;
            keys["INS"] = 0x2D;
            keys["DELETE"] = 0x2E;
            keys["DEL"] = 0x2E;
            keys["PLUS"] = 0xBB;
            keys["MINUS"] = 0xBD;
            keys["COMMA"] = 0xBC;
            keys["PERIOD"] = 0xBE;
            keys["SLASH"] = 0xBF;
            keys["SEMICOLON"] = 0xBA;
            keys["QUOTE"] = 0xDE;
            keys["BACKTICK"] = 0xC0;
            keys["LBRACKET"] = 0xDB;
            keys["RBRACKET"] = 0xDD;
            keys["BACKSLASH"] = 0xDC;
            return keys;
        }
    }

    internal static class ShortcutSender
    {
        private const uint INPUT_KEYBOARD = 1;
        private const uint KEYEVENTF_KEYUP = 0x0002;
        private const uint KEYEVENTF_EXTENDEDKEY = 0x0001;

        public static void Send(Shortcut shortcut)
        {
            List<INPUT> inputs = new List<INPUT>();

            for (int i = 0; i < shortcut.Modifiers.Length; i++)
            {
                AddKey(inputs, shortcut.Modifiers[i], false);
            }

            for (int i = 0; i < shortcut.Keys.Length; i++)
            {
                AddKey(inputs, shortcut.Keys[i], false);
            }

            for (int i = shortcut.Keys.Length - 1; i >= 0; i--)
            {
                AddKey(inputs, shortcut.Keys[i], true);
            }

            for (int i = shortcut.Modifiers.Length - 1; i >= 0; i--)
            {
                AddKey(inputs, shortcut.Modifiers[i], true);
            }

            INPUT[] inputArray = inputs.ToArray();
            uint sent = SendInput((uint)inputArray.Length, inputArray, Marshal.SizeOf(typeof(INPUT)));
            if (sent != inputArray.Length)
            {
                // There is nowhere useful to surface this from the hook path.
            }
        }

        private static void AddKey(List<INPUT> inputs, ushort virtualKey, bool keyUp)
        {
            INPUT input = new INPUT();
            input.type = INPUT_KEYBOARD;
            input.U.ki.wVk = virtualKey;
            input.U.ki.wScan = 0;
            input.U.ki.dwFlags = (keyUp ? KEYEVENTF_KEYUP : 0) | (IsExtendedKey(virtualKey) ? KEYEVENTF_EXTENDEDKEY : 0);
            input.U.ki.time = 0;
            input.U.ki.dwExtraInfo = IntPtr.Zero;
            inputs.Add(input);
        }

        private static bool IsExtendedKey(ushort virtualKey)
        {
            switch (virtualKey)
            {
                case 0x21: // PageUp
                case 0x22: // PageDown
                case 0x23: // End
                case 0x24: // Home
                case 0x25: // Left
                case 0x26: // Up
                case 0x27: // Right
                case 0x28: // Down
                case 0x2D: // Insert
                case 0x2E: // Delete
                case 0x5B: // Left Win
                case 0x5C: // Right Win
                case 0xA3: // Right Control
                case 0xA5: // Right Alt
                    return true;
                default:
                    return false;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct INPUT
        {
            public uint type;
            public InputUnion U;
        }

        [StructLayout(LayoutKind.Explicit)]
        private struct InputUnion
        {
            [FieldOffset(0)]
            public KEYBDINPUT ki;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct KEYBDINPUT
        {
            public ushort wVk;
            public ushort wScan;
            public uint dwFlags;
            public uint time;
            public IntPtr dwExtraInfo;
        }

        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    }
}
