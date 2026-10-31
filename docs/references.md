# 资料来源

本项目独立编写 C++ 和图形资源，未复制参考项目源文件、界面或厂商脚本。

- https://github.com/yszyszysz233/2.4G-battery-viewer — HID 策略参考。
- https://github.com/SgZincanya/ChargeDock — 托盘、状态与刷新语义参考。
- https://github.com/Logitech/cpg-docs/blob/master/hidpp20/README.rst — 官方 HID++ 格式与功能发现。
- https://github.com/pwr-Solaar/Solaar/blob/master/lib/logitech_receiver/hidpp20.py — 电池函数与回复。
- https://github.com/pwr-Solaar/Solaar/blob/master/docs/devices/G502%20X%20PLUS%204099.txt — G502 功能表。
- https://github.com/Sapd/HeadsetControl/blob/master/lib/devices/steelseries_arctis_nova_7.hpp — Nova 7 Gen 2 字段。
- https://github.com/Sapd/HeadsetControl/blob/master/lib/devices/protocols/steelseries_protocol.hpp — Nova 状态命令。
- https://github.com/oitsukiii/betop-battery/blob/main/docs/protocol.md — 北通家族线索，盘古单独验证。
- 本机 BetopGame 1.5.0：resources/app/20261029.asar 内 /pgv1.js，盘古 PID、FF80 接口、getBaseInfo 与字段。仅只读查阅，未复制厂商代码或资源。

系统 API：
- https://learn.microsoft.com/en-us/windows/win32/shell/notification-area
- https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/hid-architecture
- https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input
- https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex2

图标代码原创，使用 assets/brand.svg 同一配色。无外部素材；字体为系统 Microsoft YaHei UI。
