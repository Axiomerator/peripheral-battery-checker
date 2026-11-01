Peripheral Battery — Windows 11 x64 便携版

解压后运行 PeripheralBattery.exe，无需安装或额外运行时。
程序会在 EXE 旁创建 data/settings.ini。请放在有写入权限的固定目录。

设置中扫描设备，按需开启“登记”和“显示”。
托盘悬停看速览，单击固定速览，双击打开控制中心，右键打开设置或退出。
X 只收起窗口并保留托盘；完全退出请使用托盘右键菜单。
设备卡片可拖动排序；前三台已登记且显示的设备对应托盘三根电量条。
只有设备列表滚动，刷新间隔和开机自启固定在设置页底部。

升级：先退出旧版，再替换 EXE，保留 data 文件夹。
自启使用当前用户的 Windows 启动文件夹，登录后以托盘模式运行。
Win+R 输入 shell:startup 可核对 Peripheral Battery 快捷方式；关闭自启会移除它。
无需重启检查：从托盘退出程序后，双击该快捷方式，应重新出现托盘图标。
移动目录后，请在新位置重新开启自启，更新程序路径。
启动时若设备已休眠且没有本次读数，先显示“—”，操作设备后再读取。

已真机验证：
Logitech G502 X PLUS 接收器（046D:C547）
SteelSeries Nova 7 Gen 2（1038:227E）
北通盘古 BTP-PG01A（20BC:5162）

源码、完整说明与问题反馈：
https://github.com/Axiomerator/peripheral-battery-checker
许可证：MIT，见 LICENSE.txt。
