# Making a release

A release is one zip on GitHub (the DLL, the mod file, the installer, a readme) plus the same mod file on the Steam
Workshop. Every installed copy of the mod checks the repository's latest GitHub release once per start and tells the
player on the main menu when a higher version is out.

1. **Raise the version**: `kModVersion` in [`src/net.cpp`](../src/net.cpp), e.g. `"0.9"` → `"0.9.1"`. The update
   check only offers *higher* numbers, and `make_release.ps1` refuses to overwrite an existing release zip.
2. **Build both parts** (see [BUILDING.md](BUILDING.md)):
   ```
   .\mod\make_mod_sources.ps1
   .\mod\build_mod.ps1
   .\build.ps1
   ```
3. **Pack it**:
   ```
   .\tools\make_release.ps1
   ```
   It checks that the DLL and the mod file belong together and aren't older than their sources, then writes
   `release\BanishedMultiplayer-<version>.zip`.
4. **Test the zip** on a clean install: unpack it, run Install.bat, start a game with a second PC or instance.
5. **Publish on GitHub**: Releases → *Draft a new release* → tag `v<version>` (e.g. `v0.9.1`) → attach the zip →
   the release title is shown to players as the update note (e.g. "Fixes trading between towns").
6. **Update the Workshop item** with the same `bmpShare.pkm`: copy `mod\bmpShare.pkm` into `Banished\WinData`, start
   Banished, open **Mods**, click the **...** button of the mod → **Add to Workshop** the first time, **Update Mod on
   Workshop** afterwards (pick the right item). The game needs the kit's build output (`mod\work`, `mod\bin`) still on
   disk to upload. Text for the Workshop page: [WORKSHOP.md](WORKSHOP.md).

Players need **both** new files whenever `mod\` changed: the DLL warns about a `bmpShare.pkm` from another build, and
a host refuses players whose DLL differs (every rebuild of the DLL counts as a different version).

Banished itself warns when a save was made with a different version of a mod; that's expected after an update.
