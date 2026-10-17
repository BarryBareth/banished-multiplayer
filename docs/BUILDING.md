# Building

Two parts are built separately:

| | Built with | Output |
|---|---|---|
| The DLL (all the code) | Visual Studio's C++ compiler | `build\dinput8.dll` |
| The mod file (windows, buttons, texts, icons) | the Banished Mod Kit | `mod\bmpShare.pkm` |

They must come from the same sources: the mod file carries a version marker (a hash of its sources), and the DLL warns
players whose `bmpShare.pkm` doesn't match. `src\modversion.h` (written by `mod\make_mod_sources.ps1`) holds the
marker the DLL expects, and it's committed, so you can build **just the DLL** without the mod kit as long as you
don't change anything under `mod\`.

## What you need

- Windows 10/11 and **PowerShell 7** (`pwsh`).
- **Visual Studio 2022** (Community is fine) with the **Desktop development with C++** workload.
- **Banished** (Steam, 1.0.7) to run and test it.
- For the mod file only: the **Banished Mod Kit** 1.0.7 from Shining Rock Software, set up as its readme describes
  (it needs the game's `.pkg` files in its `bin\WinData` folder).
- Python 3 for the test tools.

Paths are found automatically (Banished through Steam's library list, Visual Studio through `vswhere`). Where that
fails, or for the mod kit, set them as environment variables or in a `local-paths.txt` file in the repository root
(ignored by git):

```
BANISHED_DIR=D:\SteamLibrary\steamapps\common\Banished
VCVARS64=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
BANISHED_KIT=D:\BanishedKit
```

## The DLL

```
.\build.ps1            # -> build\dinput8.dll
.\build.ps1 -Deploy    # and copy it into the Banished folder
```

Players must all run the **same DLL file**: the version check compares a hash of the DLL's code, and a DLL you build
yourself (another compiler version, another checkout, even other line endings) almost never matches the official
release byte for byte. To play with people on the release, use the release's DLL; to test your own build, give the
other players your DLL.

`build.ps1` also writes `src\repo_gen.h` from `repo.txt` (`owner/name` of the GitHub repository: the update check
and the "get the DLL" texts point there). Forks: change `repo.txt`.

## The mod file

```
.\mod\make_mod_sources.ps1   # generates mod\src from the kit's resources + mod\static (and src\modversion.h)
.\mod\build_mod.ps1          # builds mod\bmpShare.pkm with the kit's tools
.\build.ps1 -Deploy          # rebuild the DLL: it must know the new mod version
```

Copy `mod\bmpShare.pkm` into `Banished\WinData` to use it. `mod\work` and `mod\src` are generated and contain copies
of the game's own resource files from the mod kit; they are not part of the repository and must not be published.
The mod's own files are in `mod\static` (its windows: `Dialog\Bmp*.rsc`, icons: `Dialog\Sprite\Bmp`) and in
`mod\make_mod_sources.ps1` (changes to the game's storage, workplace and main-menu windows, texts).

## Logs

The DLL writes `Banished\bmp.log` (the previous run's log is kept as `bmp_prev.log`). Crashes: the game writes
`crash.dmp`; `python -I tools\minidump.py crash.dmp` shows the exception and stack, and
`python -I tools\sym.py build\dinput8.dll <offset>` turns DLL offsets into source lines (needs `build\dinput8.pdb`
from the same build).
