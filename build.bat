@echo off
rem PeekStock 构建脚本 — 产物: PeekStock.exe (静态链接绿色单文件, 含图标)
rem 需要 MinGW-w64 (本机: E:\dev\tools\msys64\ucrt64\bin), 或 PATH 中已有 g++

where g++ >nul 2>nul
if errorlevel 1 (
    if exist "E:\dev\tools\msys64\ucrt64\bin" set PATH=E:\dev\tools\msys64\ucrt64\bin;%PATH%
)

rem 图标资源 (重新设计图标: python make_icon.py)
windres app.rc -O coff -o app.res
if errorlevel 1 ( echo 资源编译失败 & exit /b 1 )

g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -municode -mwindows main.cpp app.res -lgdiplus -lwinhttp -limm32 -lshell32 -lole32 -luuid -lgdi32 -o PeekStock.exe
if errorlevel 1 ( echo 编译失败 & exit /b 1 )
echo 构建完成: PeekStock.exe
