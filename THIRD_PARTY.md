# Third-party code and assets

| | License | Where |
|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) 1.91.5 | MIT | `third_party/imgui` (see its `LICENSE.txt`) |
| [MinHook](https://github.com/TsudaKageyu/minhook) | BSD 2-Clause | `third_party/minhook` (see its `LICENSE.txt`) |

Banished, its game files and its mod kit are © Shining Rock Software LLC. This repository contains none of their
files: the build scripts generate the mod's resources from your own copy of the Banished Mod Kit, and the DLL only
refers to the game's code by address. The Steamworks interfaces used by `src/steam.cpp` are declared by hand; no
Steamworks SDK files are included.
