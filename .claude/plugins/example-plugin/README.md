# Example Plugin

This is an example plugin for cxxode demonstrating the plugin system.

## Contents

- **skills/example-skill.md** - An example skill
- **commands/** - (empty, but could contain custom commands)
- **agents/** - (empty, but could contain subagent configs)

## Installation

Copy this directory to `~/.ode/plugins/` or `~/.claude/plugins/`:

```bash
cp -r .claude/plugins/example-plugin ~/.ode/plugins/
```

Or symlink it for development:

```bash
ln -s $(pwd)/.claude/plugins/example-plugin ~/.ode/plugins/example-plugin
```

## Usage

After installation, restart ode. The `example-skill` will be available:

```
> ode "Use the example-skill"
```

## Plugin Structure

See docs/M4_FEATURES.md for complete documentation on creating plugins.
