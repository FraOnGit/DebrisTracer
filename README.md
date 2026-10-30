# Debris Tracer

A [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) mod for **HITMAN World of Assassination** that shows where
the pieces of broken objects go, and measures what happens under 47's feet when debris carries him upwards.

It was made to study the "violin boost": when 47 stands on the pieces of something that breaks (a violin, a vase,
a breaching charge's debris), PhysX can carry him up through the ceiling. Debris Tracer draws the pieces, follows
them, and reads what the physics engine itself says about them.

## Features

- **Track the fragments around 47**: every new piece of debris within a radius you choose gets a line along its
  path. Pieces that were already in the level when it started are left out, so only real debris is drawn.
- **Track 47**: a line along 47's own path.
- **Show the real boxes** of the pieces near 47, as PhysX has them.
- **Give 47 violins** (1 to 20 at a time) and **Infinite ammo**, for testing. Infinite ammo turns off again when you
  untick it, restart the mission or load a new level.
- **REMOVE ALL FRAGMENTS** around 47: sends the debris far under the map with its gravity off, to start a test
  afresh. The game's own objects are not destroyed.

### Measuring tools

Behind the **Measuring tools** tick, each tool is off (and costs nothing) until you switch it on:

- **Timers**: what each part of the mod costs per frame.
- **PhysX state**: what PhysX has 47 standing on, its step counter and the ride state, live; it can also be
  written to the log for 10 seconds.
- **Describe the piece under 47**: mass, size, and where PhysX touches it.
- **Collide check**: asks the game's own collision filter whether 47's body and the pieces near him can touch.
- **Probe the set-down under 47**: runs the sweep the game uses to put 47 back on his feet after a ride, and says
  whether it lands on the piece or goes through it. Nothing is moved.
- **Ray from his feet**: what 47 stands on, every frame.
- **Log rockets**: every tracked piece that moves very far in one frame goes to the log, with its mass, size,
  owner and where it was against 47.
- **Record boosts**: the frames before and after every time 47 rises suddenly are written to the log.
- **Launch**: puts a piece under 47 and sends it up, the way the engine itself carries him. The sliders control
  how high, how many tries, and how the piece is placed.

Everything the mod writes goes to the window and to `%LOCALAPPDATA%\DebrisTracer.log` (it starts again empty
once it passes 20 MB).

## Requirements

- HITMAN World of Assassination (tested on version 3.290.1, Steam).
- [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK/releases) 4.2.1.

## Install

1. Install ZHMModSDK.
2. Copy `Retail/mods/DebrisTracer.dll` from the zip into `HITMAN 3/Retail/mods/`.
3. Start the game, open the SDK menu (`~` key), click **MODS**, tick **DebrisTracer** and press **OK**.
4. In the SDK menu bar click **DEBRIS TRACER** to open the window.

**Uninstall:** disable it in the Mods list, or delete `DebrisTracer.dll` (and `debristracer.ini`, its settings)
from `Retail/mods/`.

## How it works

- The fragments are found through PhysX's own list of the scene's moving bodies, and each one is recognised as a
  piece of debris by the game's class for it. Nothing has to be walked over or shot to be found.
- What 47 stands on is read from his PhysX character controller, the same object the game moves him with.
- The carry itself is PhysX's "ride on the touched object": each time PhysX moves 47 it looks at what he touched
  last, and if that has moved up it moves him up by as much. It only happens when PhysX holds the piece as his
  footing **and** the physics scene has taken a step since it last looked. The launch waits for that moment.
- Every object is checked by its class before anything is read from it or called on it, so a piece the game
  destroys in the meantime is simply dropped.

The game addresses the mod uses were found with Ghidra and are specific to one game version. On any other version
the window says **"Unknown game build"**: the tools that read the game may stop working, and **Collide check** is
switched off.

## Troubleshooting

- **"Unknown game build"**: the game has been updated and the mod needs an update.
- **No lines appear**: check that **Enabled** and **Track the fragments around 47** are ticked. Fragments are noted
  a few seconds after a level starts; before that nothing is tracked.
- **"PhysX: no controller for 47"**: wait a second after loading, it is looked for once per second.
- The mod's own log is `%LOCALAPPDATA%\DebrisTracer.log`; the SDK log is `HITMAN 3/Retail/ZHMModLoader.log`.

## Building from source

The mod is built inside the ZHMModSDK source tree:

1. Clone [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) and follow its build instructions.
2. Copy this repository's `CMakeLists.txt` and `Src/` into `Mods/DebrisTracer/` of the SDK.
3. Add `DebrisTracer` to the `set(MODS ...)` list in the SDK's root `CMakeLists.txt`.
4. Build the `DebrisTracer` target (`cmake --preset x64-Release`, then
   `cmake --build _build/x64-Release --target DebrisTracer`).

## Credits

- [ZHMModSDK](https://github.com/OrfeasZ/ZHMModSDK) by Orfeas Zafeiris. The violin is given the way the SDK's
  Assets mod gives items, and the infinite ammo follows the SDK's Player mod.
- PhysX 3.4 by NVIDIA, whose published source helped read the game's own build of it.
- Made by Fra47. The code was written with Claude Code (Opus 5.5), with the game's internals studied in Ghidra and
  checked in game.

## License

GPL-3.0, the same license as ZHMModSDK. See `LICENSE`.
