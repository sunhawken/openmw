Morrowind with the melee combat of Dark Messiah of Might and Magic, built on OpenMW 0.51.0.
You need your own copies of Morrowind and Dark Messiah of Might and Magic.

This is an alpha release: the combat works, the game balance is not tuned yet.

## Fixed in 0.2.1

* **Linux startup:** the launcher, game and helper programs find their bundled libraries when
  the `.x86_64` files are started directly, including from another working directory. This fixes
  the missing Boost library error reported in #4; installing a different system Boost is unnecessary.
* **Qt plugins:** the launcher and installation wizard find their own X11 and Wayland plugins.
  Missing plugin dependencies are included, and unused embedded-display and compositor plugins
  have been removed.
* **Game plugins:** OpenSceneGraph plugins are found relative to the game executable, so starting
  the game from a shortcut or another folder works.
* **Importer runtime:** completed the Linux Python library dependencies and kept the bundled
  Python isolated from the game's libraries. GCF and VPK imports were verified through the launcher.

Linux packages now pass automated startup checks on clean **Fedora 45** and **Ubuntu 24.04**
environments, including X11 and Wayland windows, the importer runtime and game initialization.

## Updating

Existing imports and settings from **0.2.0** are reused. You do not need to import Dark Messiah again.
Replace the unpacked Linux package or run the updated Windows installer.

## Downloads

* **Linux** (Debian 13, Ubuntu 24.04 with updates, Fedora 45 or newer, x86-64):
  `BrutalMorrowind-0.2.1-Linux-64Bit.tar.gz`. Unpack it anywhere and run `brutalmorrowind-launcher`.
  Requires glibc 2.38+, a GCC 14-compatible C++ runtime and desktop OpenGL libraries.
* **Windows** (Windows 10 or newer, 64-bit): `BrutalMorrowind-0.2.1-Windows-x64.exe`. Run the installer.

For Steam and Steam Deck, **Steam Linux Runtime 4.0** is the recommended compatibility tool
for the native Linux package. The older scout, soldier and sniper containers are not supported.
Proton is for the Windows package.

On the first run the launcher imports Dark Messiah's animations, sounds and textures from your copy
of the game; this takes about half a minute.

See the README in the repository for requirements, the first run and the FAQ.
