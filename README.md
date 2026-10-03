# fs25vr: stereoscopic 6DOF VR for Farming Simulator 25

**Mod for Farming Simulator. Unofficial: not affiliated with or endorsed by GIANTS Software.**
Free and open source. Use at your own risk.

fs25vr turns Farming Simulator 25 into a real VR game through OpenXR. It works with SteamVR, Oculus/Meta, Windows Mixed Reality, Virtual Desktop and other runtimes.

- **True stereoscopic 3D.** Every eye is rendered from its own position with its own projection. It is not a depth effect and not a flat screen floating in front of you.
- **Full 6DOF head tracking.** Lean, duck and look around inside the cab. On foot, you turn with the mouse or stick and your head adds look, lean and crouch.
- **Smooth headset pacing.** The headset runs at its own refresh rate on a separate thread, so the game never waits for it. Each eye's image is shown with the exact head pose it was rendered from.
- **Synchronised eyes.** The simulation holds still between the left and right image of a pair, so moving vehicles and turning stay solid.
- **Comfortable menus.** Menus, the map and the shop appear on a flat screen in front of you, with a visible mouse pointer.
- **HUD hidden in VR** (F10 brings it back). Your desktop window stays usable and can be larger than your monitor.

## Requirements

- Farming Simulator 25 on Windows with the **DirectX 12** renderer (the default). Tested with v1.24.0.0, v1.20.0.0 and v1.5.0.1, including the official Steam exe.
- A PC VR headset and an OpenXR runtime set as active (for example SteamVR's OpenXR setting).

## Install

1. Download the latest release zip from https://github.com/nick10180/FS25VR/releases and unpack it anywhere.
2. Double-click **`INSTALL.bat`**. It finds the game through Steam, the Windows installed-programs list or Epic, and asks for the folder if it can't.
   - If Windows says *"Windows protected your PC"*, click **More info**, then **Run anyway**.
   - Or install manually: copy everything in `x64\` into `<game>\x64\` and `mod\FS25_VR.zip` into your mods folder. If you copy by hand, **unblock the DLLs** (see the [FAQ](#faq)).
3. Start the game with your headset connected. Enable **VR (OpenXR, stereoscopic 6DOF)** in the mod list when you load a savegame.

To uninstall, double-click **`UNINSTALL.bat`**, or delete `dinput8.dll`, `openxr_loader.dll` and `fs25vr.ini` from `<game>\x64` and remove the mod.

### Recommended setup

- **Render size.** After your first VR session, quit the game and double-click **`SET VR RESOLUTION.bat`**. It reads the ideal size for your headset from `<game>\x64\fs25vr.log`, switches the game to a window of that size and turns vsync off. Your old `game.xml` is backed up, and the window is shrunk to fit your monitor automatically.
- **In the game's graphics settings:** turn off **FSR3 frame generation**, **DLSS/DSR** and **motion blur**. Alternate-eye rendering confuses effects that blend across frames. Ambient occlusion and screen-space reflections work.
- **Frame rate.** Each eye updates at half the game's frame rate. Aim for at least your headset's refresh rate in game fps. The log reports the frame rate and GPU time every 10 seconds.

## Keys

| Key | Action |
|---|---|
| F8 | Recentre (sit or stand in your neutral position first) |
| F9 | VR camera on/off (flat screen in the headset) |
| F10 | HUD on/off in VR (hidden by default) |
| F6 | Projection: centred (default) / exact off-centre |
| F7 | Ambient occlusion: game setting (default) / force SAO |

## Settings (`<game>\x64\fs25vr.ini`)

| Setting | Meaning |
|---|---|
| `symmetricFrustum` | 1 (default): each eye renders a centred frustum enclosing its field of view. The game's ambient occlusion assumes a centred projection, so with the exact off-centre frustum it differs between the eyes. On headsets with offset lenses this costs horizontal pixel density; use the recommended render size to compensate. |
| `asyncSubmit` | 1 (default): the headset frame loop runs on its own thread |
| `syncEyes`, `syncPhase` | Freeze the simulation on right-eye frames. If moving objects still look doubled, try `syncPhase=1`. |
| `worldScale` | >1 makes the world feel smaller |
| `menuDistance`, `menuWidth` | Placement of the flat menu screen (metres) |
| `fitWindow`, `clipMouse`, `showCursor` | Desktop window fitting, keeping the mouse in the window, pointer in the headset |
| `presentLag` | Starting guess for frame latency; measured automatically at VR start |
| `profile`, `debugLog` | Diagnostics: GPU timing, a per-frame CSV, verbose logging |

## How it works

| Part | Role |
|---|---|
| `dinput8.dll` (bridge) | A proxy DLL loaded by the game. It hooks the game's D3D12 swap chain, runs an OpenXR session on the game's own GPU device, copies each finished frame into the headset's eye images, and gives the Lua mod a small VR API. |
| `FS25_VR` mod | Every frame it asks the bridge which eye to draw. It then places the game camera at that eye's exact position and orientation, with that eye's field of view. |

Eyes are rendered alternately: one game frame per eye. Each image goes to the headset with the pose it was rendered from, and the OpenXR compositor re-aligns both eyes to your current head pose every refresh.

## Known limitations

- **Each eye updates at half the game's frame rate.** Head motion is smooth because the runtime re-aligns each eye to your head, but world motion updates at the per-eye rate.
- **Effects that blend in the previous frame** may show artefacts, because the previous frame belongs to the other eye. Frame generation and DLSS/DSR are the known ones.
- **The HUD can't be shown on the desktop mirror while it's hidden in the headset.**
- **Multiplayer:** works if the server has the mod, but has had little testing.
- **Game updates** can stop the bridge from finding the engine functions it needs. It then logs this and falls back to a flat screen; it won't crash.

## FAQ

### I can see the mod in the list in game, but it stays a flat screen and none of the keys (F8, F9, F10…) respond

Windows has most likely **blocked the DLLs**. Files downloaded from the internet are marked *"This file came from another computer and might be blocked"*. On some PCs, Windows then refuses to let the game load `dinput8.dll` and `openxr_loader.dll`, because they are not digitally signed. The mod loads, but the part that does VR never starts.

To fix it, close the game and unblock both files:

1. Go to the game's `x64` folder (for example `...\steamapps\common\Farming Simulator 25\x64`).
2. Right-click **`dinput8.dll`** → **Properties**.
3. At the bottom of the **General** tab, tick **Unblock**, then click **OK**. If there is no Unblock box, the file isn't blocked.
4. Do the same for **`openxr_loader.dll`**.

Or do both at once from PowerShell:

```powershell
Get-ChildItem "D:\path\to\Farming Simulator 25\x64\dinput8.dll", "D:\path\to\Farming Simulator 25\x64\openxr_loader.dll" | Unblock-File
```

`INSTALL.bat` does this automatically from version 0.1.2. It mostly happens after copying the files by hand, or when unzipping with a tool that keeps the block mark.

### Windows Defender says `dinput8.dll` is "Trojan:Win32/Wacatac" (or another antivirus flags it)

That's a **false positive**. The `!ml` at the end of names like `Wacatac.C!ml` means it's a guess by Microsoft's machine-learning model, not a match against known malware. The model distrusts what every VR injector (and tools like ReShade) has to do: an unsigned DLL placed next to a game that hooks the game's graphics. Other engines on VirusTotal report it as clean. The source is fully open in this repository, and each release lists SHA-256 checksums so you can check your copy is the official one.

If Defender quarantined the file, VR won't start. To restore it: **Windows Security → Virus & threat protection → Protection history**, select the entry → **Actions → Restore**. Optionally, add an exclusion for the game's `x64` folder. You can also help by reporting it to Microsoft as incorrectly detected at https://www.microsoft.com/en-us/wdsi/filesubmission.

### How do I know whether the VR part is running?

Open `<game>\x64\fs25vr.log`. If the file doesn't exist at all, the DLL never loaded: it was blocked (see above) or isn't in the `x64` folder. If it exists, it lists each step: engine hooks found, swap chain, OpenXR runtime and session, frame rate and GPU time. The game's own `log.txt` should also contain `[FS25_VR] native bridge v1 connected`.

### Everything is a flat screen in the headset, but the keys work

The game shows menus, the map, the shop and loading screens on a flat screen on purpose. In a savegame, check that **VR (OpenXR, stereoscopic 6DOF)** is ticked in the mod list, and press **F9** in case the VR camera was switched off.

### The image stutters or looks doubled when I turn or drive

Keep the game's frame rate high: ideally above your headset's refresh rate, since each eye updates at half the game's frame rate. Make sure frame generation (DLSS, XeSS or FSR), DLSS/DSR upscaling and motion blur are **off**. If moving objects still look doubled, set `syncPhase=1` in `fs25vr.ini`.

### Is the DLL safe?

The full source code is in this repository, and every release lists SHA-256 checksums (`SHA256SUMS.txt` in the zip). It does not modify any game files. Deleting the three files from the `x64` folder removes it completely.

### Something else is wrong

Open an [issue](https://github.com/nick10180/FS25VR/issues) and attach `<game>\x64\fs25vr.log`.

## Building from source

Requirements: Visual Studio 2022 or later (C++ desktop workload), CMake 3.20+, Ninja.

1. Download the OpenXR SDK loader package (`openxr_loader_windows-<ver>.zip` from https://github.com/KhronosGroup/OpenXR-SDK-Source/releases) and unpack it into `third_party/openxr/`.
2. Run `build.bat` (edit the `vcvars64.bat` path inside it for your Visual Studio). The output is `build/dinput8.dll`.
3. `install.ps1` installs from the build. `package.ps1` creates `release/fs25vr-<version>.zip` with SHA-256 checksums.

## License

fs25vr is released under the MIT License (see `LICENSE`). The OpenXR loader is Apache-2.0 (see `THIRD_PARTY_NOTICES.md`). This project contains no Farming Simulator or GIANTS Engine code or assets, and it does not modify game files on disk.
