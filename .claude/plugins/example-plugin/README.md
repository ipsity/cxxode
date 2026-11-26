# Example Plugin

This is an example plugin for cxxode demonstrating the plugin system.

## Contents

- **skills/example-skill.md** is an example skill.
- **ode/init.lua** registers the `/lua-example` command.

## Installation

From the repository root in PowerShell, copy the plugin into your ode profile.

```powershell
$plugins = Join-Path $env:USERPROFILE '.ode\plugins'
New-Item -ItemType Directory -Force -Path $plugins | Out-Null
Copy-Item -Recurse -Force .claude\plugins\example-plugin -Destination $plugins
```

Start `ode` and enter `/lua-example`. The command prints `Lua 插件已加载` without calling a model. Use `/reload` after editing the installed script.

See [Lua 扩展](../../../docs/ARCHITECTURE.md#6-lua-扩展) for the available Lua APIs.
