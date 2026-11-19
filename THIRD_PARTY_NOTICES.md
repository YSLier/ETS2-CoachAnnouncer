# 第三方来源及许可文件

## SCS Telemetry SDK

用于读取 ETS2 遥测。包内 SDK 头文件和原许可位于 `third_party/scs_sdk`。官网所列公开 SDK 为 1.16。

- https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry
- 原许可文件为 `third_party/scs_sdk/sdk_license.txt`。

## SPF Framework

交互面板采用官方公开 C API。包内 API 头文件来自 v1.2.6，完整运行包附框架 v1.2.7，版本来源见 `third_party/spf/README.md`。原 Apache-2.0 许可文件位于 `third_party/spf/LICENSE`，运行包附 SPF-LICENSE.txt。

- https://github.com/TrackAndTruckDevs/SPF-Framework
- https://github.com/TrackAndTruckDevs/SPF-Framework/releases/tag/v1.2.7

## Truck Dash / Nethercap

标准地图站点表由公开 POI 导出的 `busStations`、`busTouristStops`、`viewpoints` 字段转换而来。固定提交为 `731cea845d3968e7e14a42105b2e7fd3142a5d14`。

地图数据的原始说明见仓库根目录 `NOTICE-STATION-MAP.txt`，附带的上游 AGPL-3.0 文本见 `TRUCKDASH-LICENSE.txt`。转换脚本位于 `tools/Import-Station-Map.ps1`。

- https://github.com/Nethercap/truck-companion
- https://github.com/Nethercap/truck-companion/blob/731cea845d3968e7e14a42105b2e7fd3142a5d14/docs/data/pois-ets2.json

游戏运行时读取已安装的本地坐标表。

## 编译工具链运行库

发布 DLL 使用 LLVM-MinGW 构建。包内保存的 LLVM、MinGW-w64 和 Winpthreads 声明位于 `third_party/build_runtime`。

## 安装器

EXE 由 NSIS 构建，源码位于 `installer/setup.nsi`。

- https://nsis.sourceforge.io/

## 语音素材

运行资源包包括预生成 WAV 和提示音，未附离线 TTS 模型。当前归档未完整记录每段素材的声线生成条件及授权信息，维护者仍需补充相应说明。本文未为这些素材声明统一的商业使用授权。

## 主项目许可

当前整理包未选择主项目整体许可证。维护者决定授权方式后，应补充根目录 LICENSE，并更新 README。第三方来源和原许可文件应随其对应组件保留。
