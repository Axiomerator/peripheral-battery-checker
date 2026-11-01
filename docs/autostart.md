# Windows 登录自启

自启作用于当前 Windows 用户，在**登录桌面后**以托盘模式运行，不需要管理员权限。使用 Windows 启动文件夹中的原生 Shell Link：

    shell:startup → Peripheral Battery.lnk

程序通过 SHGetKnownFolderPath(FOLDERID_Startup) 取得真实路径，使用 IShellLinkW / IPersistFile 创建快捷方式。目标是当前 EXE，参数为 --tray，工作目录和图标均指向便携程序所在目录。支持中文及带空格的路径。

## 开关行为

- 开启：先写临时文件并读回检查，再替换正式快捷方式；检查 EXE、参数、工作目录及 Windows 禁用状态，成功后显示开启。
- 关闭：删除快捷方式，并清理旧版 HKCU Run 中名为 PeripheralBattery 的注册项。重复关闭也能成功。
- 显式开启或关闭只清理本程序对应的 Explorer StartupApproved 缓存值，由 Windows 重新维护状态；不修改其他启动项，不写入自造的二进制批准记录。
- 从旧版升级：正常启动时，将指向当前 EXE 且未被系统禁用的旧 Run 项迁移为快捷方式。
- 重新打开或激活控制中心会读取系统实际状态；无法写入或删除时显示系统错误，避免只切换按钮。
- 移动 EXE 后，需要在新位置重新开启自启，更新目标路径。

Windows 自启可受组织策略或安全软件限制。任务管理器中关闭本程序后，应用会在下次激活控制中心时显示关闭；在应用内明确重新开启，会重新注册本程序的启动项。

## 无需重启的快速检查

1. 在程序设置里开启“开机自启”。
2. 按 Win+R，输入 shell:startup。应能看到 Peripheral Battery 快捷方式。右键属性：目标应为实际程序路径，参数为 --tray，“起始位置”为程序所在文件夹。
3. 从托盘菜单**退出**程序。窗口上的 X 只隐藏窗口，不能用来验证重新启动。
4. 在启动文件夹中双击该快捷方式：应重新出现托盘图标，主窗口不自动弹出。
5. 在程序设置中关闭自启：启动文件夹中的快捷方式应立即消失；再次开启后应重新出现。

这能验证注册、路径、托盘启动及移除。**完整的 Windows 登录链路**仍需注销后重新登录验证，无需重启电脑；注销会关闭应用，请先保存工作。登录后可等待约一分钟，并检查托盘折叠菜单。Windows 可能延迟执行启动文件夹中的项目。

任务管理器的“启动应用”也可用于核对；列表显示可能需要重新打开任务管理器。启动文件夹里的实际快捷方式及其目标更适合直接核查。

## 命令行核对

PowerShell 中运行，路径换成实际 EXE：

    $exe = 'D:\Apps\PeripheralBattery\PeripheralBattery.exe'
    (Start-Process -FilePath $exe -ArgumentList '--autostart status' -Wait -PassThru).ExitCode

退出码 0 表示当前 EXE 的启动项有效且未处于已识别的 Windows 禁用状态；3 表示未启用或路径、参数不匹配。

手动注册或移除：

    (Start-Process -FilePath $exe -ArgumentList '--autostart on' -Wait -PassThru).ExitCode
    (Start-Process -FilePath $exe -ArgumentList '--autostart off' -Wait -PassThru).ExitCode

这两条命令会**实际改变当前用户的自启配置**，成功返回 0，失败返回 1；参数错误返回 2。它们与界面开关使用相同的注册和移除实现。命令执行完即退出，不启动监视进程。

## 回归测试

CTest 的 autostart_regression 在指定测试目录及 HKCU\Software\PeripheralBattery\Tests 下的进程独占键中检查创建、读回、错误目标 / 参数、Windows 禁用状态、重新开启、旧项清理、重复关闭与写入失败。使用含中文和空格的 EXE 路径；结束时清理测试键和测试快捷方式。

    PeripheralBattery.exe --test-startup --data-dir 指定隔离目录

它不会注册真实的登录启动项。未指定隔离目录时返回 5。

## 平台依据

- [Microsoft：配置 Windows 启动应用](https://support.microsoft.com/en-us/windows/experience/startup-boot/configure-startup-applications-in-windows)
- [Microsoft：Shell Links](https://learn.microsoft.com/en-us/windows/win32/shell/links)
- [Microsoft：SHGetKnownFolderPath](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetknownfolderpath)
