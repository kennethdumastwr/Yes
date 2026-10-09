# Show Tap — Android source project

Show Tap draws a dot at a configurable screen position whenever a touch begins. It does **not** draw at the actual finger position.

## Settings

- **X Position (%)**: 0 = left, 100 = right. Default 80.
- **Y Position (%)**: 0 = bottom, 100 = top. Default 43, slightly below the vertical center.
- **Dot Size / Dot Opacity / Dot Lifetime**: control the appearance and duration.
- **Shakiness**: adds a random offset around the configured X/Y location. Set to 0 for a fixed position.

Coordinates are percentages of the active Geometry Dash viewport, so the default location scales to different screen sizes. The underlying Cocos coordinate system has its origin at the bottom-left.

## Build for Android

The project must be compiled with the Geode SDK and Android NDK. The `.geode` package cannot be generated from inside Geometry Dash itself.

### If you have a computer

1. Install the Geode CLI, Geode SDK, and Android NDK; configure `ANDROID_NDK_ROOT` as required by the official Geode docs.
2. Open a terminal in this folder and run:

   ```bash
   geode sdk install-binaries -p android64
   geode build -p android64
   ```
3. The output should be in `build-android64/`. Copy the `.geode` file to:

   `/storage/emulated/0/Android/media/com.geode.launcher/game/geode/mods/`

4. Open/restart Geometry Dash with Geode. If it does not appear, check that your Geode Launcher and game version match the mod's target version.

### If you only have your Android phone

A cloud build is usually easier than installing a full C++/NDK toolchain on the phone:

1. Create a GitHub repository and upload the contents of this project (make sure `mod.json`, `CMakeLists.txt`, `src/`, and `.github/` are at the repository root).
2. Open the repository's **Actions** tab and run **Build Show Tap for Android** (or push a commit to trigger it).
3. When the workflow finishes, download the `showtap-android64` artifact from that workflow run and extract the `.geode` file.
4. Install the `.geode` file using the Geode Launcher/import flow if available, or copy it to the mods path above. Restart the game.

Official docs: https://docs.geode-sdk.org/getting-started/create-mod/

## Build status

This source has not yet been compiled or tested against your exact device/game installation. If a build fails, share the complete error log so the source can be corrected.
