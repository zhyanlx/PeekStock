@echo off
rem DesktopStockView (原生 Win32 版) 构建脚本 — 产物: DesktopStockView.exe (静态链接绿色单文件)
rem 需要 MinGW-w64 (本机: E:\dev\tools\msys64\ucrt64\bin), 或 PATH 中已有 g++

where g++ >nul 2>nul
if errorlevel 1 (
    if exist "E:\dev\tools\msys64\ucrt64\bin" set PATH=E:\dev\tools\msys64\ucrt64\bin;%PATH%
)

g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -municode -mwindows main.cpp -lgdiplus -lwinhttp -limm32 -lshell32 -lole32 -luuid -lgdi32 -o PeekStock.exe
if errorlevel 1 ( echo 编译失败 & exit /b 1 )
echo 构建完成: DesktopStockView.exe
