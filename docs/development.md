# 构建与开发

## 工具

- Windows x64。
- Visual Studio 2022 或 Build Tools 2022：使用 C++ 的桌面开发、Windows SDK。
- CMake 3.24+，在 PATH 中。
- Git 用于版本管理；Python 3 只用于重新生成应用图标。

源码不携带编译器、SDK 或第三方运行时。应用使用系统 Win32 / HID / GDI+ API 和静态 C++ 运行库。

## 日常构建

    powershell -ExecutionPolicy Bypass -File tools/build.ps1

脚本通过 vswhere 定位 VS 2022，配置 CMake、编译 Release 并运行 CTest；EXE 位于 dist/。

独立构建：

    powershell -ExecutionPolicy Bypass -File tools/build.ps1 -BuildDirectory build/review -OutputDirectory build/review/bin

Release 输出位置由 PBC_OUTPUT_DIR 指定；UI 测试目录由 PBC_TEST_DATA_DIR 指定。脚本把测试数据放在对应的构建目录。UI 回归使用单独宿主窗口，不占用日常程序的单实例互斥量，也不查询真实 HID。

## 验证

CTest 包含自启快捷方式隔离检查、协议解析 / 休眠缓存检查，以及十二设备模拟界面的排序、固定通用设置、滚动、窗口缩放和关闭到托盘检查。测试输出位于构建目录的 ui-regression/。

    ctest --test-dir build -C Release --output-on-failure

硬件充电状态、休眠 / 唤醒和系统恢复需要真机验证。已完成范围见 [验证记录](validation.md)。

## 图形

assets/ 中的 SVG、ICO 与图形绘制代码为原创。重新生成 ICO：

    python tools/generate_icon.py

C++ 格式配置为 .clang-format。Windows SDK 头文件存在依赖顺序，配置保留 include 顺序。

## 诊断参数

| 参数 | 用途 |
| --- | --- |
| --probe 文件.json | 主动查询支持设备并保存诊断快照，绕过托盘进程的静置缓存 |
| --data-dir 目录 | 指定独立设置目录 |
| --tray | 仅托盘启动 |
| --settings | 打开设置 |
| --popup | 首次读取后打开速览 |
| --capture 文件.png | 首次读取后保存真实窗口截图，可配合 --settings / --popup |
| --verify-layout --data-dir 隔离目录 | 十二设备界面回归，不查询真实 HID |
| --verify-controls | 登记、显示、间隔写入恢复检查，应使用隔离目录且退出常驻实例 |
| --autostart on / off / status | 注册、移除或核对当前用户启动项，执行后退出；详见 [自启说明](autostart.md) |
| --test-startup --data-dir 隔离目录 | 原生快捷方式与启动状态回归，不操作真实登录启动项 |
| --quit | 退出现有常驻实例 |

--probe 只用于诊断；日常使用由托盘进程管理静置保护。

## 新增设备

先确认 VID/PID、HID usage、报文长度及电量字段，记录来源与真机证据，再添加协议匹配与解析。设备识别和查询位于 src/hid.cpp；偏好、顺序和窗口逻辑分别位于 settings.cpp、main.cpp。只向已匹配的厂商接口发送确定的状态查询。
