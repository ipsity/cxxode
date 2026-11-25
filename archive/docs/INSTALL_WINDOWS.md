# Windows 手动安装指南

## 方法 1：最简单的方法（推荐）

### 步骤 1：复制文件到用户目录
```bash
# 在 cxxode 目录运行
mkdir %USERPROFILE%\bin
copy build\release\ode.exe %USERPROFILE%\bin\ode.exe
```

### 步骤 2：添加到用户 PATH（不需要管理员权限）
```bash
# 方法 A：使用 setx（推荐）
setx PATH "%PATH%;%USERPROFILE%\bin"

# 方法 B：手动设置
# 1. Win + R 输入 sysdm.cpl
# 2. 高级 -> 环境变量
# 3. 在"用户变量"中找到 Path
# 4. 点击"编辑"，添加：C:\Users\你的用户名\bin
# 5. 确定保存
```

### 步骤 3：重启终端并测试
```bash
# 关闭当前终端，打开新终端
ode --version
```

## 方法 2：使用当前目录（临时）

```bash
# 在 cxxode 目录
cd build\release

# 直接运行（不需要安装）
.\ode.exe --version

# 添加当前目录到 PATH（临时，重启失效）
set PATH=%PATH%;D:\cxxode\build\release

# 现在可以在任何地方运行
cd D:\任意目录
ode --version
```

## 方法 3：创建符号链接（需要管理员）

```powershell
# 以管理员身份运行 PowerShell
# 创建符号链接到 System32
New-Item -ItemType SymbolicLink -Path "C:\Windows\System32\ode.exe" -Target "D:\cxxode\build\release\ode.exe"
```

## 方法 4：使用简化的安装脚本

创建 `quick-install.bat`：
```batch
@echo off
echo 简易安装 cxxode...
set BIN_DIR=%USERPROFILE%\bin
if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"
copy /Y build\release\ode.exe "%BIN_DIR%\ode.exe"
setx PATH "%PATH%;%BIN_DIR%"
echo.
echo 安装完成！请重启终端，然后运行: ode --version
pause
```

双击运行 `quick-install.bat`（不需要管理员权限）

## 验证安装

```bash
# 重启终端后
where ode
# 应该显示: C:\Users\你的用户名\bin\ode.exe

ode --version
# 应该显示: ode 0.2.0

# 测试运行
ode "hello"
# 如果没有配置 API 密钥，会提示：
# ode: HTTP 401: ...
```

## 配置 API 密钥

```bash
# 方法 1：使用 login 命令
ode login anthropic
# 输入: sk-ant-...

# 方法 2：设置环境变量（永久）
setx ANTHROPIC_API_KEY "sk-ant-..."
# 重启终端生效

# 方法 3：临时设置（当前终端）
set ANTHROPIC_API_KEY=sk-ant-...
```

## 故障排查

### 问题 1："ode 不是内部或外部命令"

**原因：** PATH 没有生效

**解决：**
```bash
# 1. 完全关闭所有终端窗口
# 2. 重新打开终端
# 3. 检查 PATH
echo %PATH%
# 应该包含 C:\Users\你的用户名\bin

# 4. 如果还是不行，手动运行
D:\cxxode\build\release\ode.exe --version
```

### 问题 2："访问被拒绝"

**原因：** 没有管理员权限或文件被占用

**解决：**
```bash
# 使用方法 1（用户目录，不需要管理员）
# 或关闭所有 ode 进程
taskkill /F /IM ode.exe
```

### 问题 3：脚本执行策略错误

**解决：**
```powershell
# 以管理员身份运行 PowerShell
Set-ExecutionPolicy RemoteSigned -Scope CurrentUser
```

## 推荐方法（最可靠）

**复制到用户 bin 目录：**
```bash
# 1. 创建目录
mkdir %USERPROFILE%\bin

# 2. 复制文件
copy build\release\ode.exe %USERPROFILE%\bin\

# 3. 添加到 PATH（只需运行一次）
setx PATH "%PATH%;%USERPROFILE%\bin"

# 4. 重启终端
# 5. 测试
ode --version
```

这个方法：
- ✅ 不需要管理员权限
- ✅ 不会被其他程序影响
- ✅ 升级时只需重新复制文件
- ✅ 在所有终端都可用（CMD、PowerShell、Git Bash）
