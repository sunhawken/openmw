# Brutal Morrowind

Brutal Morrowind is Morrowind with the melee combat of Dark Messiah of Might and Magic: kicks,
directional and charged attacks, parries, sliding, adrenaline finishers, ragdolls and dismemberment,
with Dark Messiah's own first-person animations. It is a fork of [OpenMW](https://openmw.org) 0.51.0.

No game files are included. Brutal Morrowind uses the games you own.

## Requirements

* Morrowind.
* Dark Messiah of Might and Magic, for its animations, sounds and effects. Without it the game runs
  with Morrowind's animations and the features that need Dark Messiah's are turned off.
* Linux: Debian 13, Ubuntu 24.04 with updates, Fedora 45 or a newer distribution,
  x86-64 (glibc 2.38+, GCC 14-compatible C++ runtime and desktop OpenGL libraries).
* Windows: Windows 10 or newer, 64-bit.

## Installation

Download the package for your system from the Releases page.

* Linux: unpack `BrutalMorrowind-<version>-Linux-64Bit.tar.gz` anywhere and run
  `brutalmorrowind-launcher` in the unpacked folder.
* Windows: run `BrutalMorrowind-<version>-Windows-x64.exe` and start Brutal Morrowind from the
  Start menu.

On Steam Deck, add the launcher as a non-Steam game and select Steam Linux
Runtime 4.0 as its compatibility tool. The Linux package requires a newer glibc
than the scout, soldier and sniper containers provide. Keep the whole unpacked
folder together; Qt and Boost are bundled.

## First run

1. The launcher opens the installation wizard. Choose your Morrowind folder.
2. Open the Dark Messiah page. The launcher finds a Steam installation of Dark Messiah by itself;
   otherwise choose its folder. Steam VPK archives, older GCF archives (including those
   inside `mm/`) and an already extracted `mm/` folder are supported. VPK directory
   archives may have any name ending in `_dir.vpk`; Steam depot names are not required.
3. Press Play. The first time, the launcher imports Dark Messiah's animations, sounds and textures
   (about half a minute) and then starts the game.

Settings, saves and the imported data are kept apart from OpenMW's: in `~/.config/brutalmorrowind`
and `~/.local/share/brutalmorrowind` on Linux, in `Documents\My Games\Brutal Morrowind` on Windows.

## FAQ

### 1. Is this vibe-coded AI slop?

This mod was created with extensive use of AI. You can call it whatever you like if it amuses you.

It's just a small personal project, and I use the tools that I like and find suitable. I'm not trying to deceive anyone or convince anyone to use them.

So if you hate AI or believe that using AI during mod development is unacceptable, please just ignore this project.

### 2. How does it work? Is it one of those AI merge/passthrough mods where you run two games simultaneously?

This is a modified version of the open-source OpenMW engine that uses animations imported from Dark Messiah of Might and Magic. No passthrough.

It runs just like the normal OpenMW engine. Dark Messiah files are only used as a source of animations and resources.

### 3. Do I really need to have my own copy of Dark Messiah? Why?

You need your own installed copy of the game. Animations and resources from Dark Messiah are not included in this repository for obvious legal reasons.

The mod will automatically import all the necessary resources from the game files on startup.

### 4. Why do you need these engine modifications? Wasn't Lua scripting enough?

This is a conscious choice. OpenMW's Lua scripting capabilities give you a lot of possibilities, but they were not enough to implement everything planned (e.g. procedural dismemberment) for this mod.

If you think this is the wrong way to do it, feel free to create your own version in whatever way you like.

### 5. Are you planning an MWSE version?

Only OpenMW is supported.

MWSE is great, but it does not provide access to the proprietary Gamebryo engine itself. I chose OpenMW because it's open-source and can be modified directly.

### 6. Can you play through the entire game like in normal Morrowind, or is this just a staged demo?

Yes you can. The full Morrowind game is still here. You can start a new game, create a new character, and play normally.

Apart from the combat modifications, it's just OpenMW.

### 7. What about balance? Doesn't this mod just make your character OP? How does it integrate with Morrowind's RPG mechanics?

This mod definitely breaks the game's balance and makes your character OP.

In the current alpha release, I focused mostly on importing and integrating Dark Messiah's animations and physics. Balance is something to work on in future iterations. It's not there yet.

If you care about natural character progression and balance, it's better to use other combat mods for now.

### 8. I've never played Morrowind before and want to play it for the first time with this mod. Is that a good idea?

I would definitely not recommend that.

This is not some lore-friendly, perfectly balanced mod. Morrowind's RPG system was not designed for this kind of action-style combat. In its current state, this mod will break the game's balance.

Keep that in mind.

### 9. Is the magic system also changed? Do you have any plans for magic?

Currently, only melee combat has been implemented (swords, shields, daggers, and staves).

Even two-handed swords still use Morrowind's original animations because there were no suitable animations for two-handed weapons in Dark Messiah.

### 10. I only want ragdoll physics / FPV / (something else) from the mod, but I don't want the other features. Is that possible?

Yes. In the **Scripts → Brutal Morrowind** section of the game settings, you can enable or disable mod features independently.

Basically, with all features disabled, you can get just the normal vanilla openmw experience.

### 11. Is Brutal Morrowind compatible with X mod?

It depends. Brutal Morrowind is just regular OpenMW with some changes.

So content and graphics mods that are compatible with the original OpenMW should work just fine. However, if a mod changes game physics, animations, camera, combat, or controls, there is a risk of conflicts.

You'll have to check compatibility yourself.

## Building from source

Linux, in the same container as the release build (Docker or Podman):

```sh
docker build -t brutal-linux -f CI/brutal/linux.Containerfile CI/brutal
docker run --rm -v "$PWD:/src" -w /src brutal-linux bash CI/brutal/build-linux.sh /src/build-linux /src/dist
```

The package is written to `dist/`.

Windows: the installer is built by `.github/workflows/windows.yml`.

Any other build follows the [OpenMW build instructions](https://wiki.openmw.org/index.php?title=Development_Environment_Setup).
The Dark Messiah importer also needs its Python runtime:

```sh
python3 tools/dm-import/package_python.py <dir>
cmake -DBRUTAL_PYTHON_RUNTIME=<dir> ...
```

## Credits and license

Brutal Morrowind is licensed under the GNU General Public License version 3 (see `LICENSE`).
It is built on OpenMW (see `AUTHORS.md`) and uses code and libraries from the projects listed in
`CREDITS.md`. It was developed with the help of AI.

Morrowind is a trademark of Bethesda Softworks. Dark Messiah of Might and Magic is a trademark of
Ubisoft. Brutal Morrowind is not affiliated with either.
