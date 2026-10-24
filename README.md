# ObsTwitchPlugin

OBS Studio plugin that adds a Twitch integration dock.

## Twitch OAuth setup

Before the plugin can update channel info or connect to Twitch chat, create a Twitch application and authorize it:

1. Open the Twitch Developer Console: https://dev.twitch.tv/console/apps
2. Create an app or use an existing app.
3. Add `http://localhost:38471` as an OAuth Redirect URL for the app.
4. Copy the app's Client ID and Client Secret into the plugin's **Stream Info** tab.
5. Click **Authorize in Browser** in the plugin and approve the requested scopes, including `channel:manage:broadcast`, `chat:read`, `chat:edit`, and `moderator:read:followers`.
6. After Twitch redirects back to OBS, the plugin stores the access token, refresh token, and expiry locally. It automatically renews the access token shortly before expiry (and when OBS starts with an expired token), saves Twitch's rotated refresh token, and reconnects chat after renewal.

If you authorized with an older plugin version, click **Authorize in Browser** once more to obtain a refresh token. Manually pasted or OBS-provided access tokens alone cannot be refreshed. If Twitch revokes authorization, authorize again.

## Chat moderation

In the Chat tab, use these commands in the message box (moderation is performed through Twitch Helix APIs, not IRC):

- `/ban username [reason]`, `/unban username`, `/timeout username [seconds] [reason]` (default timeout: 600 seconds)
- `/clear`, `/delete message-id`
- `/slow [seconds]`, `/slowoff`, `/followers [minutes]`, `/followersoff`
- `/subscribers`, `/subscribersoff`, `/emoteonly`, `/emoteonlyoff`, `/uniquechat`, `/uniquechatoff`

Reauthorize Twitch once to grant `moderator:manage:banned_users`, `moderator:manage:chat_messages`, and `moderator:manage:chat_settings`. Your Twitch account must be a broadcaster/moderator in the channel. Twitch returns errors in the Chat tab when an action is not permitted.

## Custom commands and variables

The **Commands** tab supports triggers such as `wins`, `hello`, `!time`, or `win*`. An exclamation mark is not required, but **a prefix included in the configured trigger is mandatory**. The trigger can appear anywhere in a chat message, case-insensitively, as a complete word or phrase. For example, `wins` matches `How many wins today?` but not `windows`; `!wins` matches `How many !wins today?` but not `How many wins today?`. An unprefixed `wins` trigger does not consume a `!wins` message. The `*` wildcard matches **zero or more non-space characters**, so `win*` matches `win`, `windows`, and `winning`. Wildcard-only triggers are not allowed. When several triggers match, the earliest occurrence wins, using the longest match if they begin at the same position. The `{{ args }}` variable contains text after the matched trigger. Responses support:

| Variable | Meaning |
| --- | --- |
| `{{ username }}` | Display name of the person who triggered the command |
| `{{ time_now }}` | Local time, `HH:mm:ss` |
| `{{ date_now }}` | Local date, `YYYY-MM-DD` |
| `{{ channel }}` | Current Twitch channel login |
| `{{ args }}` | Text after the trigger |
| `{{ random_number }}` | Fresh random integer from 1 to 100 on every expansion |
| `{{ counter1 }}` ... `{{ counter9 }}` | Current value of a legacy numbered counter (0 if not yet created) |
| `{{ wins }}` (or another named counter) | Current value of the named counter created in the Commands tab |
| `{{ wins_home }}` | Home score from the Scoreboard tab |
| `{{ wins_away }}` | Away score from the Scoreboard tab |

Example trigger: `wins`. Example chat message: `How many wins?`. Example response: `{{ username }} — Current wins: {{ wins }}. Lucky number: {{ random_number }}!`

Counters persist between OBS restarts. **Running a chat command never changes a counter**; only the admin's increment/decrement, edit, or reset controls change its value. Repeated occurrences of a counter within one response use the same stored number. Random values are generated separately for each `{{ random_number }}` occurrence. Unknown placeholders remain unchanged.

### Counter management

The Commands tab includes a **Counters** table. Choose **Add Counter** to create a unique variable name such as `wins`, `losses`, or `deaths`. Names must begin with a letter and contain only lowercase letters, digits, and underscores. The reserved built-in names (`username`, `time_now`, `date_now`, `channel`, `args`) cannot be used.

Each counter row has **−**, **+**, **Edit**, **Reset**, **Copy**, and **Delete** buttons. Edit accepts non-negative whole numbers; Reset and Delete ask for confirmation. Copy places the `{{ name }}` variable on your clipboard. Counter values and deletions persist across OBS restarts. Deleted counters remain inactive until added again, including the old numbered counters. The variable name `random_number` is reserved.

### Scoreboard tab

The dedicated **Scoreboard** tab contains two permanent counters: `wins_home` and `wins_away`. Each has its own score display and **−**, **+**, and **Edit** controls, plus a shared **Reset Both Scores** button. They cannot be deleted or replaced with custom counters, and they do not appear in the general counter table. Both scores persist across OBS restarts. Chat commands can display them using `{{ wins_home }}` and `{{ wins_away }}`, but chat activity cannot change the scores.

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
cmake --install build
```

The built module will be produced in the `build` directory as `obstwitchplugin` (platform-specific extension).

On Fedora 44, the default install location is `~/.config/obs-studio/plugins/obstwitchplugin/bin/64bit/obstwitchplugin.so`. Override `CMAKE_INSTALL_PREFIX` if you need a different OBS plugin directory.
