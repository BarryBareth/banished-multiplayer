#pragma once
#include <string>

// Update check: once per game start, a background GET of the newest GitHub release of the project (repo.txt;
// tag "v0.9.1" = version, its page = where Download goes, its title = notes). Also reads our own format
//   {"version": "0.9.1", "url": "https://...", "notes": "..."}   (tests, or a server of your own via updateurl=)
// Settings (<game>\bmp_settings.txt): updatecheck=0 switches it off, updateurl=... points elsewhere (tests).
namespace update {
void Start(const std::string& myVersion);  // e.g. "0.9" (the release number, not the build id)
struct Info { bool newer = false; std::string version, url, notes; };
Info Get();                                // newer = a higher release number than mine is out
int Compare(const std::string& a, const std::string& b);  // numeric per dot-part: -1 / 0 / 1
}
