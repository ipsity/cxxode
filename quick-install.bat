@echo off
REM 简易安装脚本（不需要管理员权限）

echo cxxode 简易安装程序
echo.

REM 检查可执行文件
set EXE_PATH=
if exist "build\release\ode.exe" (
    set EXE_PATH=build\release\ode.exe
    echo [√] 找到 release 版本
) else if exist "build\dev\ode.exe" (
    set EXE_PATH=build\dev\ode.exe
    echo [√] 找到 dev 版本
) else (
    echo [X] 未找到 ode.exe
    echo.
    echo 请先编译:
    echo   cmake --preset release
    echo   cmake --build --preset release
    echo.
    pause
    exit /b 1
)

REM 创建用户 bin 目录
set BIN_DIR=%USERPROFILE%\bin
echo [*] 安装目录: %BIN_DIR%

if not exist "%BIN_DIR%" (
    mkdir "%BIN_DIR%"
    echo [√] 创建目录成功
)

REM 复制文件
echo [*] 复制文件...
copy /Y "%EXE_PATH%" "%BIN_DIR%\ode.exe" >nul
if %errorLevel% neq 0 (
    echo [X] 复制失败
    pause
    exit /b 1
)
echo [√] 文件已复制

REM 添加到用户 PATH
echo [*] 添加到 PATH...
setx PATH "%PATH%;%BIN_DIR%" >nul 2>&1
if %errorLevel% equ 0 (
    echo [√] PATH 已更新
) else (
    echo [!] PATH 更新可能失败，请手动添加: %BIN_DIR%
)

echo.
echo ========================================
echo 安装完成！
echo ========================================
echo.
echo 下一步：
echo   1. 重启终端（让 PATH 生效）
echo   2. 运行: ode --version
echo   3. 配置: ode login anthropic
echo.
echo 文档: docs\USAGE.md
echo.
pause
