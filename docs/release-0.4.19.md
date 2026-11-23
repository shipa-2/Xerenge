0.4.19
======

This is not a major update. It is a big optimization and bugfix release, with a
lot of quality-of-life features that have waited their time.

The game comes built: the installer only needs your own Burnout Revenge disc
image (the European or the American release, see `SETUP.md`). Run the installer
again over an installed copy to update it; your settings and game files are kept.

Downloads
---------

**Windows:** download `Burnout_Revenge_Installer-win-amd64.exe` and run it.

**Linux:** download `Burnout_Revenge_Installer-x86_64.AppImage`, then

```bash
chmod +x Burnout_Revenge_Installer-x86_64.AppImage
./Burnout_Revenge_Installer-x86_64.AppImage
```

**Android** (arm64, Adreno or Mali): install `Burnout_Revenge-android-arm64.apk`.
It opens as the installer: pick your disc image. A Mesa Turnip driver .zip is
optional for Adreno phones whose own driver misbehaves. The fixed signing key
lets later builds install over earlier ones while keeping your saves.

**Xbox One** (Dev Mode): install `Xerenge-uwp.msix` and `xerenge.cer`, then set the app type to **Game** in Dev Home. The first run asks for the disc
image and installs it into developer storage.

**macOS** (Apple silicon): download `Burnout_Revenge_Installer-macos-arm64.zip`,
unzip it and run the app inside. It puts `Burnout Revenge.app` in `~/Applications`,
so the game starts from Launchpad or Spotlight. The app is signed ad hoc, so allow
it under System Settings, Privacy & Security, Open Anyway on the first launch.

**Russian** (a mod): unzip `Xerenge-mod-locale_rus.zip` into the installed game's
`game/mods/` and choose RUSSIAN in Driver Details > Settings > LANGUAGE. See
`MODDING.md` to translate further or make a mod of your own.

**Online play is still work in progress.** The installer points at the server
(`94639.snk.wtf`) and games can be played across networks and VPNs, but the
online experience is not finished. It is planned to be fully fixed in the next
release.

What's new in 0.4.19
--------------------

- **Xbox One support and a major optimization pass.** The signed UWP package
  installs in Dev Mode, with D3D12 rendering and Jaguar-specific builds. Texture,
  memory, worker, movie-decoding and per-draw overheads were cut, with a light
  race reaching a measured, locked 60 fps and fewer stalls and freezes.
- **Much better custom music.** The stock Song Manager has a native
  **X / Choose folder** button, large libraries play correctly across pages,
  sequential and random order work, and MP3/WMA metadata is reliable. Folder
  picking works on Linux, Windows, Xbox One and Android.
- **No full-library imports on Android.** A small URI index replaces copying;
  songs open only as needed, with six recent tracks cached for playback.
  Nested folders enable recursive scanning automatically.
- **Music during crashes and takedowns.** Enable `crash_music` or **MUSIC IN
  CRASHES/TAKEDOWNS** in Settings > HACKS to bypass the encounter's volume fade.
  Replay Camera moved from LB to X to avoid conflicting with previous-track.
- **Keyboard split-screen uses the gamepad's profile.** Player 2 on the keyboard
  now shares the primary gamertag, profile settings and saves.
- **Android controls and restart improved.** The touch D-pad is now a symmetric
  rounded cross with sliding direction changes, and the restart popup really
  relaunches the game.
- **Title updates on desktop and Android.** Install and Update download both
  XboxUnity packages and skip those already installed.
- **Faster, more reliable builds everywhere.** Rendering overhead fell on PC as
  well as Xbox, CI covers all supported platforms, and the Windows/UWP toolchain
  and package freshness issues were fixed.
