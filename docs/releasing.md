# 发布与低频维护

主分支为 main。源码、原创资源和文档进入 Git；便携程序通过 GitHub Releases 分发。

## 内容边界

| 位置 | 是否纳入 Git | 用途 |
| --- | --- | --- |
| src/、assets/、tools/ | 是 | 源码、原创图形、构建 / 打包工具 |
| docs/、README、LICENSE、CHANGELOG | 是 | 用户文档、协议依据、MIT 许可证、版本记录 |
| .github/、CMake、格式与 Git 配置文件 | 是 | 自动构建和仓库设置 |
| build/、out/ | 否 | CMake、编译中间文件、测试产物 |
| dist/、releases/ | 否 | 本地程序、运行配置、生成的便携 ZIP |
| verification/、local/ | 否 | 本机原始探测、测试截图、工作笔记 |
| docs/images/settings.png | 是 | 单独选出的应用窗口截图 |

软件不携带编译器、SDK、厂商客户端、字体或外部运行时。发布脚本按固定清单写 ZIP，不从工作目录递归打包。

## 本地生成便携版

    powershell -ExecutionPolicy Bypass -File tools/package.ps1

脚本在 build/package/ 独立构建并运行测试，检查 CMake / EXE 版本一致，输出：

- releases/PeripheralBattery-版本-windows-x64.zip
- 同名 .zip.sha256
- releases/release-notes.md

ZIP 根目录只有 PeripheralBattery.exe、README.txt、LICENSE.txt。个人 data/settings.ini 不进入 ZIP，首次运行自行创建。SHA256 用于核对下载是否完整。

## 后续发版

1. 修改代码，更新 CHANGELOG.md。
2. 将 CMakeLists.txt 的项目版本与 assets/app.rc 中数字、字符串版本一起更新，例如 0.1.1。
3. 运行 tools/package.ps1，进行需要的真机验证。
4. 提交并推送 main，等待 Windows build and draft release 构建通过。
5. 创建并推送对应标签：

       git tag -a v0.1.1 -m "Release v0.1.1"
       git push origin v0.1.1

6. 标签会触发 Windows 构建 / 测试、打包，并建立 Release **草稿**。在 GitHub Releases 核对说明、ZIP 与 SHA256 后点击 Publish release。

普通 main / PR 构建只保留 14 天的 Actions 构建产物；公开下载使用 Releases 中发布的 ZIP。标签必须和源码版本一致。已发布版本需要修改时使用新版本号。

工作流由仓库自动提供的 GITHUB_TOKEN 创建草稿，无需额外配置个人令牌。构建作业只读源码，创建草稿的作业仅取得当前仓库 contents 写入权限。

## 手动发布备用方式

本地 package.ps1 通过后，在 GitHub 仓库的 Releases → Draft a new release 中选择同版本标签，粘贴 release-notes.md 内容，上传 ZIP 与 SHA256，再保存草稿或发布。无需制作 EXE 安装器。

GitHub 操作依据：[管理 Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository)、[gh release create](https://cli.github.com/manual/gh_release_create)。
