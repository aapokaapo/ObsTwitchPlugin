# ObsTwitchPlugin

OBS Studio plugin that adds a Twitch integration dock.

## Build instructions

### Prerequisites

- CMake 3.21+
- C++17 compiler
- `pkg-config`
- OBS Studio development files exposing `libobs` and `obs-frontend-api`
- Qt6 development packages for `Core`, `Widgets`, and `Network`

### Configure and build

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The built module will be produced in the `build` directory as `obstwitchplugin` (platform-specific extension).