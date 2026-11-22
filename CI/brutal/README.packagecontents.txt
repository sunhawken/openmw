=== Package contents ===

This package contains x86-64 Linux binaries of Brutal Morrowind. It runs on
Debian 13 (Trixie), Ubuntu 24.04 with updates, Fedora 45 and newer distributions.
It requires glibc 2.38 or newer, a GCC 14-compatible C++ runtime, fontconfig and
the desktop's OpenGL, X11/XCB or Wayland libraries. Qt and Boost are included;
installing distribution versions of those libraries is unnecessary.

=== Running ===

Unpack the archive anywhere and run "brutalmorrowind-launcher".

The programs locate their bundled libraries and Qt plugins relative to the
package, so the *.x86_64 files can also be run directly. Keep the entire package
together. Set DEBUG=1 when running a shell script to start it under gdb.

For a Steam Deck shortcut, select "brutalmorrowind-launcher" from the unpacked
folder. Steam Linux Runtime 4.0 is the recommended Steam compatibility tool for
this native Linux build. The scout, soldier and sniper container runtimes have
older glibc versions and are not supported by this build. Proton is for the
Windows package.

The package folder is never written to. Settings, saves and the imported Dark
Messiah data go to ~/.config/brutalmorrowind and ~/.local/share/brutalmorrowind.

=== Contents ===

brutalmorrowind            the game
brutalmorrowind-launcher   the launcher
brutalmorrowind-wizard     the Morrowind installation wizard
brutalmorrowind-import     the Dark Messiah importer, with its own Python in dm-import/
lib/                       bundled libraries and OpenSceneGraph plugins
plugins/                   Qt plugins
resources/                 engine resources and the built-in Brutal Morrowind scripts
