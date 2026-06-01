# MouseLogi

一个轻量的 Windows 鼠标快捷键映射器，用来替代只做少量按键映射时显得过重的 Logi Options+。
罗技的那个配置软件真是太贼鸡巴难用了，又重又耗资源，还登录不进去，真是莫名其妙。

## 最小资源版本

优先用 native 版本：纯 Win32 程序，没有托盘、没有窗口、没有 .NET 运行时、没有轮询。它只挂一个低级鼠标 Hook，命中配置后用 `SendInput` 发快捷键。

构建：

```powershell
.\build-native.ps1
```

运行：

```powershell
.\bin\native\MouseLogiNative.exe
```

停止：

```powershell
.\bin\native\MouseLogiNative.exe --quit
```

重启：

```powershell
.\bin\native\MouseLogiNative.exe --reset
```

启用 `CIDxxxx` 映射时不要优先用 `Stop-Process -Force`，因为它会跳过恢复 HID++ divert 的清理逻辑。若误强制结束后按钮行为异常，关闭鼠标再打开即可恢复。

配置文件在 `.\bin\native\config.ini`。修改配置后用 `--reset` 重启进程生效。

如果同一台电脑配过多只 Logitech 鼠标，可以用鼠标 ID 分块。鼠标 ID 使用蓝牙设备地址，例如当前这只 `MX Master 3` 是 `C7284491160D`：

```ini
# 全局配置，所有鼠标都使用
XButton1 = Alt+Left
XButton2 = Alt+Right

[mouse:C7284491160D]
CID0053 = RightAlt
CID0056 = Alt+Right

[mouse:DF981F4DA84A]
CID0053 = Win+Shift+S
CID0056 = Alt+Left
```

块名也可以写成 `[device:C7284491160D]` 或 `[id:C7284491160D]`。没有块名的配置仍按全局配置处理；匹配当前鼠标 ID 的配置块会按文件顺序覆盖前面的同名按键。

Logitech HID++ 特殊按钮也可以直接用 CID 配置。只有配置里出现 `CIDxxxx` 时，程序才会启用 HID++ divert：

```ini
CID0053 = Alt+Left
CID0056 = Alt+Right
CID00C3 = Win+Shift+S
```

你刚探测到的三个 CID 是 `0x0056`、`0x00C3`、`0x0053`。如果某个 CID 对应的物理键不是你想要的功能，直接交换右侧快捷键即可。也可以用别名 `LogiBack`、`LogiForward`、`LogiGesture`，但 CID 写法最明确。

检测鼠标每个键实际上报的事件：

```powershell
.\bin\native\MouseLogiProbe.exe
```

只列出 Raw Input 设备和可识别的鼠标名称/ID：

```powershell
.\bin\native\MouseLogiProbe.exe --list
```

带上目标鼠标 ID 调试。退出时会按 Raw Input 实际识别到的鼠标 ID，把本次检测到的标准鼠标事件以注释模板记录到对应配置块；如果按到的不是目标鼠标，会写入实际那只鼠标的配置块：

```powershell
.\bin\native\MouseLogiProbe.exe --mouse C7284491160D
```

打开后逐个按鼠标键。`HOOK mouse` 是低级鼠标 Hook 看到的标准事件，`RAW mouse` 是 Raw Input 鼠标事件，`RAW key` 是键盘类事件，`RAW hid` 是消费控制类 HID 事件。按 `Esc` 退出。某个键如果完全没有输出，说明它没有通过标准 Windows 鼠标/键盘事件暴露，通常要走更底层的 HID 解析。

直接读取 Logitech HID input report：

```powershell
.\bin\native\MouseLogiHidProbe.exe
```

只列出可见的 Logitech HID interface：

```powershell
.\bin\native\MouseLogiHidProbe.exe --list
```

输出中的 `deviceId=...` 就是配置块里的鼠标 ID。

HID++ 调试也可以带目标鼠标 ID。`--divert-watch` 或 `--guide` 退出时会按实际 HID++ 设备 ID，把检测到的 CID 以注释模板记录到对应配置块；如果目标 ID 不在线，会退回当前可识别的 HID++ 鼠标：

```powershell
.\bin\native\MouseLogiHidProbe.exe --divert-watch --mouse C7284491160D
.\bin\native\MouseLogiHidProbe.exe --guide --mouse C7284491160D
```

只读枚举 HID++ feature 和可重编程控制表：

```powershell
.\bin\native\MouseLogiHidProbe.exe --hidpp-info
```

临时把可 divert 的鼠标控制项切到 HID++ 通知模式并监听：

```powershell
.\bin\native\MouseLogiHidProbe.exe --divert-watch
```

按实际识别到的鼠标型号引导识别 CID：

```powershell
.\bin\native\MouseLogiHidProbe.exe --guide
```

`--guide` 会根据当前 HID++ 鼠标的 PID 选择引导步骤，例如 MX Master 3 会使用侧边横向滚轮、后退/前进键、Gesture 键和顶部可选键；MX Master 4 才会包含 Haptic Sense Panel / Actions Ring。每一步按提示先按 `Enter`，再按对应鼠标键；程序会记录第一个 HID++ CID，并把注释模板写入实际鼠标 ID 对应的配置块。

如果确实要跑旧的 MX Master 4 专用顺序，也可以显式使用：

```powershell
.\bin\native\MouseLogiHidProbe.exe --mx-master-4-guide
```

这个模式会改变当前连接期间的按钮上报方式；退出时会尝试恢复。如果程序被强制结束后按钮行为异常，关闭鼠标再打开即可恢复。

这个工具会枚举 `VID_046D` 的 HID interface，并尝试直接读取 input report。逐个按侦测不到的键，把新增的 `HID [...] data=...` 输出保存下来，就可以反推 report id 和按钮 bit 位。按 `Esc` 或 `Ctrl+C` 退出。如果没有可读接口，先关闭 Logi Options+ 后再试。

## 使用

下面是带托盘菜单的 .NET Framework 版本，方便调试或手动 Reload 配置；如果你只追求最小资源占用，可以不用它。

构建：

```powershell
.\build.ps1
```

运行：

```powershell
.\bin\MouseLogi.exe
```

运行后会出现在系统托盘。修改 `.\bin\config.ini` 后，在托盘菜单里点 `Reload Config`。

## 配置格式

每行一个映射：

```ini
XButton1 = Alt+Left
XButton2 = Alt+Right
MButton = Ctrl+W
WheelLeft = Ctrl+PageUp
WheelRight = Ctrl+PageDown
```

常用按钮名：

- `XButton1`: 后侧键，通常是 Back
- `XButton2`: 前侧键，通常是 Forward
- `MButton`: 中键
- `WheelLeft` / `WheelRight`: 横向滚轮
- `WheelUp` / `WheelDown`: 纵向滚轮
- `CID0053` / `CID0056` / `CID00C3`: Logitech HID++ 特殊按钮

常用快捷键写法：

- `Ctrl+C`
- `Ctrl+Shift+P`
- `Alt+Left`
- `Alt+Right`
- `RightAlt`
- `RightAlt+E`
- `Win+Shift+S`

## 限制

这个程序监听的是 Windows 标准鼠标事件，所以它会对所有鼠标生效，不只限制在 Logitech 鼠标上。

鼠标 ID 配置块用于启动时选择当前 Logitech HID++ 设备的配置；低级鼠标 Hook 本身不带来源设备 ID，所以 `XButton1`、`MButton` 这类标准鼠标事件仍按当前选中的配置全局生效。

MX 系列的部分特殊键如果没有被 Windows 上报为标准按钮或滚轮事件，这个轻量版本就捕获不到。那种情况需要改成 Raw Input/HID 方式，复杂度会明显高一些。
