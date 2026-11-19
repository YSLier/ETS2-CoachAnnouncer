# CoachAnnouncer

ETS2 Coaches DLC 四语客车报站插件，面向 Windows x64 和 ETS2 1.62.x。

通过 SCS Telemetry SDK 读取客运合同、当前区间和站点，提供手动广播、自动下一站、到站预告及游戏内控制面板。广播在游戏进程内播放，日常使用可离线运行。

**当前发布版本为 v0.7.3。**

[下载 v0.7.3](https://github.com/YSLier/CoachAnnouncer/releases/tag/v0.7.3)

## 功能

- 德语、英式英语、普通话、日语，支持单语和德 → 英 → 中 → 日四语连续播放。
- 发车欢迎、下一站、即将到站、终点广播四类音频，快捷键可自定义。
- 从当前客运合同匹配站点，固定语段和城市名分段拼接。
- 发车后自动播下一站，可配置途经站和终点的提前播报。
- SPF 游戏内交互面板，平时隐藏，支持线路查看、手动修正播报目标、试听、停止、音量和设置保存。
- 完整资源包提供 398 个城市或地点 ID 的四语站名音频。
- 标准地图坐标表包含 223 个线路站、56 个观光上下客点和 137 个观景点，共 416 条坐标。

398 个语音地点与 416 条地图坐标采用不同统计口径。某个城市有语音，并不代表所有地图版本的每个车站均有可靠坐标。

## 环境

- Windows x64。
- Euro Truck Simulator 2 1.62.x，已安装并启用官方 Coaches DLC。
- 使用游戏内面板需要 SPF Framework。完整包附 SPF Framework 1.2.7。
- 核心报站 DLL 可独立运行，线路数据直接来自 SCS SDK。

## 完整安装

### EXE 安装

1. 完全退出 ETS2。
2. 运行 `CoachAnnouncer-0.7.3-Setup-Win64.exe`。
3. 核对识别出的目录。应能在该目录找到 `bin\win_x64\eurotrucks2.exe`。识别失败时，在 Steam 库右键 ETS2，选择「管理 → 浏览本地文件」，将安装向导指向该目录。
4. 完成安装后启动游戏，在 SPF 插件管理中启用 `CoachAnnouncerPanel`。
5. 默认用 **左 Ctrl + 左 Shift + F8** 开关面板。

安装程序会尝试保留现有 `settings.ini` 和坐标表，覆盖 CoachAnnouncer 的音频和插件。已有 SPF 时沿用当前框架，缺少框架时安装随包版本。旧 CoachAnnouncer DLL 的备份写入 `%LOCALAPPDATA%\CoachAnnouncer\backup`。

EXE 向导目前完成了构建和解包内容核对，Windows 实机安装流程仍需进一步验证。

### ZIP 手动安装

1. 完全退出 ETS2，解压完整 ZIP。
2. 把 `01-Game` 里的 `bin` 文件夹合并到 ETS2 安装目录。
3. 在资源管理器地址栏输入 `%LOCALAPPDATA%`，把 `02-LocalAppData` 里的 `CoachAnnouncer` 文件夹合并到这里。
4. 如果之前保存过自己的配置，保留现有 `settings.ini`。自定义地图坐标表可先备份。
5. 尚未安装 SPF 时，把 `03-SPF-1.2.7` 里的 `bin` 文件夹合并到游戏目录。
6. 启动游戏并启用面板。

坐标表随包提供，复制后即可读取。

## 从旧版升级

1. 退出游戏，解压 `CoachAnnouncer-0.7.3-Offline-Update-Win64.zip`。
2. 将 `01-Game` 中的 `bin` 合并到游戏目录，覆盖主插件和面板两个 DLL。
3. 将 `02-LocalAppData` 中的 `CoachAnnouncer` 合并到 `%LOCALAPPDATA%`。
4. **同时复制 audio 文件夹**，其中有四语终点预告的整句和分段 WAV。
5. 进游戏核对面板的自动播报设置和距离阈值。

升级包的用户数据目录中只包含音频和坐标文件，已有按键、音量等配置继续使用。若使用自定义地图坐标表，可只更新 `audio` 文件夹。

## 默认按键

请打开 Num Lock。按键可在面板中修改，手柄背键可映射到相应键盘组合。

| 默认按键 | 功能 |
| --- | --- |
| Num1 | 发车欢迎，读取整条线路终点 |
| Num2 | 下一站，读取当前区间目的站 |
| Num3 | 即将到站，读取当前区间目的站 |
| Num5 | 终点广播，到达前用终点预告，到达后用结束稿 |
| 左 Ctrl + 左 Shift + F8 | 显示或隐藏游戏内面板 |

默认 `language=all`、音量 65。四语顺序为德语、英语、中文、日语。播放中的同一广播可用同键停止，面板也提供停止和试听按钮。

## 自动广播与距离

默认发车后自动播下一站。安装包中的默认配置为 `auto_approach_m=0`，需要在面板开启「启用到站前距离触发」并设定距离，例如 930 米。自动终点预告还需要开启「抵达前播报终点」。

- **途经站**采用车辆到当前目标车站的水平直线距离，再乘游戏当前地图比例。比例为 3 时，310 个世界坐标单位约换算为 930 游戏米。
- **终点**优先使用游戏导航的剩余路程，并结合已知站点坐标核验。最后一段请将导航目标保持为实际客运终点。
- 坐标判定还会检查持续接近和行驶状态，记录每个区间的播报状态以减少重复广播。
- 缺少有效坐标的途经站可手动播报，正常确认停靠后插件可以记录站点位置。

途经站的换算值仍是距离估算。匝道、绕行、高架和城市比例切换可能造成提前或推迟，设置 930 米不能保证沿实际道路还剩恰好 930 米。标准地图坐标也可能与地图 Mod 移动后的站点不同。

0.7.3 会在某个语种缺少终点预告时，尝试回退到该语种的「即将到站」音频。

## 数据目录与问题反馈

```text
%LOCALAPPDATA%\CoachAnnouncer\settings.ini
%LOCALAPPDATA%\CoachAnnouncer\audio
%LOCALAPPDATA%\CoachAnnouncer\station-map-ets2-1.62.tsv
%LOCALAPPDATA%\CoachAnnouncer\logs
```

遇到问题时，请提交 Issue 并提供游戏版本、插件版本、车型、地图 Mod、线路和出错动作。附上最近一次 `CoachAnnouncer-*.jsonl` 日志，有助于定位问题。分享前可检查日志里不希望公开的线路或本机路径信息。

自动播报日志可包含 `distance_source`、`approach_threshold_m`、`trigger_distance_m`、`world_straight_m`、`map_scale` 和 `audio_fallback`，分别用于判断距离来源、阈值、触发距离、原始坐标距离、比例及音频回退。

## 卸载与回滚

1. 完全退出游戏。
2. 移除游戏目录中的 `bin\win_x64\plugins\CoachAnnouncer.dll` 和 `bin\win_x64\plugins\spfPlugins\CoachAnnouncerPanel` 文件夹。
3. 若要回滚，将先前备份的两个 CoachAnnouncer DLL 放回对应位置。
4. `%LOCALAPPDATA%\CoachAnnouncer` 中的音频、配置和日志可按需保留或删除。
5. 其他插件仍在使用 SPF 时，保留 SPF Framework。

## 源码构建

仓库包含 C++20 源码、CMake 工程、SDK/SPF 头文件、模拟测试及安装程序脚本。运行用的音频从完整 Release 资源包获取。

Windows 下安装 Visual Studio 2022 的 C++ 桌面开发工具和 CMake 3.21+，在仓库根目录执行。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=OFF
cmake --build build --config Release
```

两个 DLL 位于 `build\Release`。已发布的 0.7.3 DLL 使用 LLVM-MinGW 交叉编译，仓库内提供 `cmake/llvm-mingw-x64.cmake`。

运行音频测试前，把完整包中 `02-LocalAppData/CoachAnnouncer/audio` 复制为仓库根目录的 `audio` 文件夹。Linux 模拟测试可执行以下命令。

```bash
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

EXE 安装脚本为 `installer/setup.nsi`，使用支持 `amd64-unicode` 的 NSIS 3。先解压完整发布 ZIP，再将 `PAYLOAD` 指向包含三个安装目录的解压目录。

```powershell
makensis /DPAYLOAD=C:\CoachAnnouncer-Payload /DOUTPUT=C:\CoachAnnouncer-0.7.3-Setup-Win64.exe installer\setup.nsi
```

## 验证范围与限制

0.7.3 已通过 Windows x64 交叉编译和五项本地模拟测试，覆盖 SDK 回调、线路、语音拼接、自动触发及 SPF 面板通信。发布包完成了校验，EXE 中的安装载荷与完整 ZIP 核对一致。

开发者收到过当前版本可用的实机反馈。全部城市、观光任务、地图 Mod 及与 MotionCab、CabPlay、Truck Dash 的所有组合尚未完成系统性实机验证。Linux 模拟测试不覆盖 Windows 真实音频设备和键盘输入。

## 第三方来源

SCS Telemetry SDK、SPF Framework、Truck Dash 站点地图导出及构建运行库的来源与声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

Euro Truck Simulator 2 及相关商标归 SCS Software 所有。
