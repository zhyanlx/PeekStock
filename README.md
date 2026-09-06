# PeekStock

一款低调的桌面行情便签，用于在工作期间以不打扰的方式查看自选股与大盘指数。原生 Win32 编写，单文件、无第三方依赖。

平时它收缩为屏幕角落的一行灰色小字，视觉上接近屏幕水印，不易引起注意；鼠标移入时展开为完整的看盘面板，`Ctrl+Q` 可随时整体隐藏。

平时（精简模式）：

![精简模式](docs/compact.png)

鼠标移入（展开模式）：

![展开模式](docs/expanded.png)

## 功能

- 无边框置顶悬浮窗，可拖动；鼠标移出自动收缩为便签，移入恢复完整面板
- 每 5 秒批量拉取一次自选股与上证指数（单次请求），断网时指数显示 `--`
- 成本价：点击「成本」输入价格，行内显示 `(持±x.xx%)`；输入留空并确认即清除
- 按当日涨跌幅升/降序排序
- 添加股票：输入 6 位代码自动识别沪深市场（6 开头为沪市），或输入中文名称自动反查
- `Ctrl+Q` 全局隐藏/唤出，隐藏期间不发送任何请求
- 请求限频：最快 5 秒一次，频繁切换隐藏/显示不会产生额外请求

配置文件位于 `%APPDATA%\PeekStock\conf.json`，保存自选股与成本价；更换设备时拷贝该文件即可迁移数据。

## 下载 / 构建

从 [Releases](../../releases) 下载 `PeekStock.exe`（约 1.6MB，静态链接，Windows 10/11 可直接运行）。

自行构建需安装 MinGW-w64（MSYS2：`pacman -S mingw-w64-ucrt-x86_64-gcc`），然后运行 `build.bat`，或手动执行：

```
g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -municode -mwindows main.cpp -lgdiplus -lwinhttp -limm32 -lshell32 -lole32 -luuid -lgdi32 -o PeekStock.exe
```

源码为单个 `main.cpp`，基于 Win32 与 GDI+，无第三方依赖。分层窗口、整窗自绘控件、输入法兼容的自绘输入框等实现细节可直接参考源码。

## 说明

- 行情数据来自腾讯（qt.gtimg.cn），证券名称反查来自新浪（suggest3.sinajs.cn），均为公开接口。数据仅供学习与研究参考，不构成任何投资建议；接口变更可能导致功能失效。
- 程序未经数字签名，首次运行时如遇 SmartScreen 或杀毒软件提示，请选择信任/放行。

## License

MIT
