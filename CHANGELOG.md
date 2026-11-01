# 更新记录

## [0.1.1] - 2026-11-01

修复 Windows 登录自启。

- 使用当前用户启动文件夹中的原生快捷方式，保存 EXE 路径、--tray 参数及工作目录。
- 开关操作后读回核验，控制中心重新激活时同步 Windows 状态，写入或移除失败时显示错误。
- 关闭自启会移除快捷方式和旧 Run 项；升级时迁移有效的旧启动项。
- 新增自启隔离回归和 --autostart on / off / status 诊断命令。

下载 **PeripheralBattery-0.1.1-windows-x64.zip**，退出旧版后替换 EXE，保留 data 文件夹。若旧版未留下有效启动项，请在新版设置中重新开启自启。无需重启的检查方法见 [自启说明](https://github.com/Axiomerator/peripheral-battery-checker/blob/main/docs/autostart.md)。

## [0.1.0] - 2026-10-31

首个公开版本。

- 支持 G502 X PLUS、Nova 7 Gen 2 和北通盘古的厂商 HID 电量读取。
- 原生托盘、缓存速览、控制中心与设备设置。
- 显示百分比、充电和静置状态；静置暂停与最后有效电量保留。
- 设备登记、显示、拖拽排序、滚动与窗口缩放。
- 通用设置固定在设置页底部；支持当前用户开机自启。
- 单个便携 EXE，静态 C++ 运行库；ZIP 不包含个人配置。

实验性支持 PRO 2 LIGHTSPEED 与 G502 USB 直连，尚未真机验证。

### 下载与运行

下载 **PeripheralBattery-0.1.0-windows-x64.zip**，解压后运行 PeripheralBattery.exe。已验证环境为 Windows 11 x64，无需安装包或额外运行时。

便携包只含 EXE、使用说明和 MIT 许可证；首次运行在 EXE 旁创建 data/settings.ini，不携带维护者的个人设备配置。升级时先退出旧版，再替换 EXE，保留 data 文件夹。

附件包含对应的 SHA256 校验文件。完整操作和设备支持范围见 [README](https://github.com/Axiomerator/peripheral-battery-checker#readme)。
