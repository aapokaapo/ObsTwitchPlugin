# ObsTwitchPlugin

OBS Studio plugin that adds a Twitch integration dock.

## Twitch OAuth setup

Before the plugin can update channel info or connect to Twitch chat, create a Twitch application and authorize it:

1. Open the Twitch Developer Console: https://dev.twitch.tv/console/apps
2. Create an app or use an existing app.
3. Add `http://localhost:38471` as an OAuth Redirect URL for the app.
4. Copy the app's Client ID and Client Secret into the plugin's **Stream Info** tab.
5. Click **Authorize in Browser** in the plugin and approve the requested scopes.
6. After Twitch redirects back to OBS, the plugin caches the returned OAuth token and uses it for chat and channel updates.

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