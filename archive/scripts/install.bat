@echo off
REM cxxode Windows 安装脚本
setlocal enabledelayedexpansion

echo 正在安装 cxxode...
echo.

REM 检查是否以管理员身份运行
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo 错误: 需要管理员权限
    echo 请右键点击此脚本，选择"以管理员身份运行"
    echo.
    pause
    exit /b 1
)

REM 检查构建文件（先尝试 release，再尝试 dev）
set EXE_PATH=
if exist "build\release\ode.exe" (
    set EXE_PATH=build\release\ode.exe
    echo 找到 release 版本
) else if exist "build\dev\ode.exe" (
    set EXE_PATH=build\dev\ode.exe
    echo 找到 dev 版本
) else (
    echo 错误: 未找到 ode.exe
    echo.
    echo 请先运行以下命令编译:
    echo   cmake --preset release
    echo   cmake --build --preset release
    echo.
    echo 或使用 dev 版本:
    echo   cmake --preset dev
    echo   cmake --build --preset dev
    echo.
    pause
    exit /b 1
)

REM 创建安装目录
set INSTALL_DIR=%ProgramFiles%\ode
echo 安装目录: %INSTALL_DIR%
if not exist "%INSTALL_DIR%" (
    mkdir "%INSTALL_DIR%"
    if %errorLevel% neq 0 (
        echo 错误: 无法创建目录 %INSTALL_DIR%
        pause
        exit /b 1
    )
)

REM 复制文件
echo 复制文件...
copy /Y "%EXE_PATH%" "%INSTALL_DIR%\ode.exe"
if %errorLevel% neq 0 (
    echo 错误: 复制文件失败
    pause
    exit /b 1
)

REM 添加到 PATH
echo 添加到系统 PATH...
set "NEW_PATH=%INSTALL_DIR%"

REM 获取当前 PATH
for /f "tokens=2*" %%a in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v Path 2^>nul') do set "CURRENT_PATH=%%b"

REM 检查是否已在 PATH 中
echo %CURRENT_PATH% | find /i "%NEW_PATH%" >nul
if %errorLevel% equ 0 (
    echo PATH 已包含 %NEW_PATH%
) else (
    echo 添加 %NEW_PATH% 到 PATH...
    setx /M Path "%CURRENT_PATH%;%NEW_PATH%" >nul
    if %errorLevel% neq 0 (
        echo 警告: 无法更新 PATH，请手动添加
        echo 添加 %NEW_PATH% 到系统环境变量 Path
    ) else (
        echo PATH 更新成功
    )
)

echo.
echo ========================================
echo 安装完成！
echo ========================================
echo.
echo 下一步:
echo   1. 重新打开终端（新的 PATH 才会生效）
echo   2. 运行: ode --version
echo   3. 配置 API 密钥: ode login anthropic
echo.
echo 然后就可以在任何目录使用 ode 了！
echo.
pause

