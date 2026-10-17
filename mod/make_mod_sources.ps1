# Generates the BMP data mod's resources from the Banished Mod Kit's originals (<kit>\resource, BANISHED_KIT in tools\paths.ps1):
# StorageBarn / StorageYard / StorageCart get a "Share" checkbox (ControlUI on the StorageComponent, control id 77 -
# the multiplayer DLL answers it) in the window's userButton0 slot. Rerun after changing anything below.
#   .\mod\make_mod_sources.ps1
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\tools\paths.ps1"
$kit = Join-Path (Get-BanishedKit) 'resource'
$src = Join-Path $PSScriptRoot 'src'
# where players get the DLL (shown on the main menu when the DLL is missing): <repo>\repo.txt, "owner/name"
$repoUrl = 'github.com/' + (Get-Content (Join-Path (Split-Path $PSScriptRoot) 'repo.txt') -TotalCount 1).Trim()
New-Item -ItemType Directory -Force "$src\Template", "$src\BMP" | Out-Null

$controller = @'
		{
			ObjectType _type = ControlUI;
			ElementDescription _element = "shareStorageButton";
			String _insertAt = "userTitle2";
			DialogControllerConfig _config = "shareStorageConfig";
		}
'@

$definitions = @'

// ---- Banished Multiplayer: share this storage building with the other players' towns (control id 77 = the DLL)
ControlUIConfig shareStorageConfig
{
	ObjectType _type = StorageComponent;
	int _controlId = 77;
}

// a small "[x] Share" check in the window's title bar (visible in the tab layout storage windows use)
CheckDescription shareStorageButton : "Dialog/SharedElements.rsc:checkSimple"
{
	ElementDescription _content = "shareStorageOff";
	ElementDescription _check = "shareStorageOn";

	Dialog _toolTipDialog = "Dialog/ToolTip.rsc";
	StringTable _toolTipStringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _toolTipText = "ShareStorageTip";
}

RibbonDescription shareStorageOff
{
	bool _vertical = false;
	Alignment _alignment = MidLeft;
	int _cellPad = 2;
	ElementDescription _elements
	[
		"Dialog/SharedElements.rsc:checkSimpleUnchecked",
		"shareStorageText",
	]
}

RibbonDescription shareStorageOn
{
	bool _vertical = false;
	Alignment _alignment = MidLeft;
	int _cellPad = 2;
	ElementDescription _elements
	[
		"Dialog/SharedElements.rsc:checkSimpleChecked",
		"shareStorageText",
	]
}

TextDescription shareStorageText
{
	Alignment _alignment = MidLeft;
	int _leftPad = 2;
	int _rightPad = 4;
	int _topPad = 3;
	int _bottomPad = 3;

	Font _font = "Font\FontSmall.rsc";
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _text = "ShareStorage";
}
'@

foreach ($name in 'StorageBarn', 'StorageYard', 'StorageCart') {
    $lines = [IO.File]::ReadAllLines("$kit\Template\$name.rsc")
    $out = New-Object System.Collections.Generic.List[string]
    $inUi = $false; $inControllers = $false; $done = $false
    foreach ($l in $lines) {
        if ($l -match '^UIDescription ui') { $inUi = $true }
        if ($inUi -and $l -match 'ElementController _controllers') { $inControllers = $true }
        if ($inControllers -and -not $done -and $l -match '^\s*\]\s*$') {
            foreach ($c in ($controller -split "`r?`n")) { $out.Add($c) }
            $done = $true; $inControllers = $false
        }
        $out.Add($l)
    }
    if (-not $done) { throw "${name}: controller list not found" }
    foreach ($d in ($definitions -split "`r?`n")) { $out.Add($d) }
    [IO.File]::WriteAllLines("$src\Template\$name.rsc", $out)
    "wrote Template\$name.rsc"
}

# main menu: a "Multiplayer" button under New / Load (element id 77 is set and handled by the DLL)
$menu = [IO.File]::ReadAllText("$kit\Dialog\MainMenu.rsc")
if ($menu -notmatch '"buttonLoad",') { throw 'MainMenu.rsc: buttonLoad not found in the button list' }
$menu = $menu -replace '("buttonLoad",)', "`$1`r`n`t`t`"buttonMultiplayer`","
# the "this mod needs the DLL" panel: last in the button list (under Exit), hidden by the DLL when the menu opens
if ($menu -notmatch '"buttonQuit",') { throw 'MainMenu.rsc: buttonQuit not found in the button list' }
$menu = $menu -replace '("buttonQuit",)', "`$1`r`n`t`t`"bmpNeedsDll`","
$menu += @'

// ---- Banished Multiplayer: opens the multiplayer window (the DLL gives this button id 77)
ButtonDescription buttonMultiplayer : "buttonMainMenu"
{
	ElementDescription _content = "labelMultiplayer";
	StringTable _toolTipStringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _toolTipText = "MultiplayerTip";
}

LabelDescription labelMultiplayer : "labelMainMenu"
{
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _text = "Multiplayer";
	String _spriteName = "NomadTab";
}

// ---- Banished Multiplayer: shown only when the DLL is missing (the DLL hides "bmpNeedsDll" when the menu opens)
GroupDescription bmpNeedsDll
{
	Alignment _alignment = MidCenter;
	int _topPad = 6;
	ElementDescription _elements
	[
		"bmpNeedsDllBorder"
		"bmpNeedsDllList"
	]
}
BorderDescription bmpNeedsDllBorder : "SharedElements.rsc:dialogBorder"
{
	Color _color = 0xD0000000;
}
RibbonDescription bmpNeedsDllList
{
	int _leftPad = 14;
	int _topPad = 10;
	int _rightPad = 14;
	int _bottomPad = 12;
	Alignment _alignment = MidCenter;
	bool _vertical = true;
	int _cellPad = 4;
	ElementDescription _elements
	[
		"bmpNeedsDllTitle"
		"bmpNeedsDllText"
	]
}
TextDescription bmpNeedsDllTitle
{
	Alignment _alignment = MidCenter;
	Font _font = "Font\FontMedium.rsc";
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _text = "NeedsDllTitle";
	Color _color = 0xFFFFD060;
	bool _dropShadow = true;
}
TextDescription bmpNeedsDllText
{
	Alignment _alignment = MidCenter;
	Font _font = "Font\FontSmall.rsc";
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _text = "NeedsDllText";
	Color _color = 0xFFE8E8E8;
	bool _wordWrap = true;
	int _wordWrapSize = 300;
}
'@
New-Item -ItemType Directory -Force "$src\Dialog" | Out-Null
[IO.File]::WriteAllText("$src\Dialog\MainMenu.rsc", $menu)
"wrote Dialog\MainMenu.rsc"

# a "[x] text" check like the storages' Share button: CheckDescription + two ribbons + text (strings in ShareStrings)
function CheckWithText([string]$name, [string]$textName, [string]$tipName) {
    return @"

CheckDescription $name : "Dialog/SharedElements.rsc:checkSimple"
{
	ElementDescription _content = "${name}Off";
	ElementDescription _check = "${name}On";
	Dialog _toolTipDialog = "Dialog/ToolTip.rsc";
	StringTable _toolTipStringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _toolTipText = "$tipName";
}
RibbonDescription ${name}Off
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 2;
	ElementDescription _elements [ "Dialog/SharedElements.rsc:checkSimpleUnchecked", "${name}Text", ]
}
RibbonDescription ${name}On
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 2;
	ElementDescription _elements [ "Dialog/SharedElements.rsc:checkSimpleChecked", "${name}Text", ]
}
TextDescription ${name}Text
{
	Alignment _alignment = MidLeft;
	int _leftPad = 2; int _rightPad = 4; int _topPad = 3; int _bottomPad = 3;
	Font _font = "Font\FontSmall.rsc";
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _text = "$textName";
}
"@
}

# event log: "Mine" / "Everyone" under the list (EventLogUI hooks in the DLL: ids, filtering, player names)
$log = [IO.File]::ReadAllText("$kit\Dialog\EventLog.rsc").Replace("`r`n", "`n")
$anchor = "`t`t{`n`t`t`tString _name = `"buttonPrevious`";"
if (-not $log.Contains($anchor)) { throw 'EventLog.rsc: layout anchor not found' }
$log = $log.Replace($anchor, @'
		{
			String _name = "bmpFilter";
			ElementDescription _element = "bmpFilter";
			LayoutHorizontal _xLayout = LeftToLeft; String _xAnchorTo = "display";
			LayoutVertical _yLayout = TopToBottom; String _yAnchorTo = "display"; int _yOffset = 4;
		}
'@.Replace("`r`n", "`n") + "`n" + $anchor)
$log += @'

// ---- Banished Multiplayer: whose messages the log (and the pop-ups) show
RibbonDescription bmpFilter
{
	bool _vertical = false;
	int _cellPad = 8;
	ElementDescription _elements [ "bmpLogMine", "bmpLogAll", ]
}
'@
$log += (CheckWithText 'bmpLogMine' 'LogMine' 'LogMineTip') + (CheckWithText 'bmpLogAll' 'LogAll' 'LogAllTip')
[IO.File]::WriteAllText("$src\Dialog\EventLog.rsc", ($log -replace "\r?\n", "`r`n"))
"wrote Dialog\EventLog.rsc"

# workplaces: "Share" in the title bar ribbon every workplace window uses (WorkPlaceUI hooks in the DLL)
$work = [IO.File]::ReadAllText("$kit\Dialog\Work.rsc").Replace("`r`n", "`n")
$old = "`t`t`"workersAndMax`"`n`t`t`"workerCount`"`n`t]"
if (-not $work.Contains($old)) { throw 'Work.rsc: workPlace ribbon not found' }
$work = $work.Replace($old, "`t`t`"workersAndMax`"`n`t`t`"workerCount`"`n`t`t`"bmpShareWork`"`n`t]")
# a title-bar icon toggle like the event log's category buttons (dim = off, lit = on): the title bar has little room
$work += @"

// ---- Banished Multiplayer: other players' villagers may work here (the DLL handles it)
CheckDescription bmpShareWork : "Dialog/EventLog.rsc:checkEventNotify"
{
	StringTable _toolTipStringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _toolTipText = "ShareWorkTip";
	ElementDescription _check = "bmpShareWorkImage";
	ElementDescription _content = "bmpShareWorkImage";
}
ImageDescription bmpShareWorkImage : "Dialog/StandardDialog.rsc:titleImage"
{
	SpriteSheet _spriteSheet = "Dialog/SpriteSheet.rsc";
	String _spriteName = "NomadTab";
}
"@
[IO.File]::WriteAllText("$src\Dialog\Work.rsc", ($work -replace "\r?\n", "`r`n"))
"wrote Dialog\Work.rsc"

@'
StringTable shareStrings
{
	Entry _strings
	[
		{ String _name = "ShareStorage";		String _text = "Share"; }
		{ String _name = "ShareStorageTip";		String _text = "Share this building with the other players: their villagers may store and take goods here."; }
		{ String _name = "Multiplayer";			String _text = "Multiplayer"; }
		{ String _name = "MultiplayerTip";		String _text = "Play with other players: host or join a game (separate towns or one shared town)."; }
		{ String _name = "TradeTitle";			String _text = "Trade"; }
		{ String _name = "OfferTitle";			String _text = "Trade offer"; }
		{ String _name = "SettleTitle";			String _text = "Choose your town's location"; }
		{ String _name = "LogMine";				String _text = "Mine"; }
		{ String _name = "LogMineTip";			String _text = "Show only your town's messages (also the pop-ups)."; }
		{ String _name = "LogAll";				String _text = "Everyone"; }
		{ String _name = "LogAllTip";			String _text = "Show every player's messages, with their name in front."; }
		{ String _name = "ShareWork";			String _text = "Share"; }
		{ String _name = "ShareWorkTip";		String _text = "Share: let the other players' villagers work here. What they make goes to your storage."; }
		{ String _name = "CopyTip";				String _text = "Copy the room code (send it to the other players)."; }
		{ String _name = "PasteTip";			String _text = "Paste the room code or address you copied."; }
		{ String _name = "ReportTip";			String _text = "Report a bug: describe what happened and save or send the report."; }
		{ String _name = "NeedsDllTitle";		String _text = "Banished Multiplayer needs one more file"; }
		{ String _name = "NeedsDllText";		String _text = "This mod does nothing on its own. Download the Banished Multiplayer program (dinput8.dll) from REPO_URL and follow the install steps there, then restart Banished."; }
	]
}
'@.Replace('REPO_URL', $repoUrl) | Set-Content "$src\BMP\ShareStrings.rsc" -Encoding ascii

# hand-written resources (the multiplayer window, ...): mod\static -> mod\src
Copy-Item -Recurse -Force "$PSScriptRoot\static\*" $src
# The other Bmp*.rsc dialogs use the multiplayer window's building blocks (text, button, check, combo, edits, title).
# Inherited elements resolve their own references (a check's "checkOff" ribbon, ...) in the INHERITING file, so the
# blocks are copied into each of them instead of being referenced across files.
$mp = [IO.File]::ReadAllText("$src\Dialog\BmpMultiplayer.rsc").Replace("`r`n", "`n")
$a = $mp.IndexOf('LabelDescription labelTitle'); $b = $mp.IndexOf("`nRibbonDescription body")
$c = $mp.IndexOf('// ---------------------------------------------------------------- building blocks'); $d = $mp.IndexOf('// ---------------------------------------------------------------- setup')
if ($a -lt 0 -or $b -lt 0 -or $c -lt 0 -or $d -lt 0) { throw 'BmpMultiplayer.rsc: building blocks not found' }
$blocks = "`n// ---- building blocks (copied from BmpMultiplayer.rsc by make_mod_sources.ps1)`n" + $mp.Substring($a, $b - $a) + $mp.Substring($c, $d - $c)
foreach ($f in Get-ChildItem "$src\Dialog\Bmp*.rsc" | Where-Object Name -ne 'BmpMultiplayer.rsc') {
    $t = [IO.File]::ReadAllText($f.FullName).Replace("`r`n", "`n").Replace('BmpMultiplayer.rsc:', '') + $blocks
    [IO.File]::WriteAllText($f.FullName, ($t -replace "\n", "`r`n"))
}
"copied static resources"

# Version marker: BMP/Version_<hash>.rsc, the hash of everything above (the mod's content). The DLL is built with the same
# hash (src/modversion.h) and looks for that file: not there = the player's bmpShare.pkm is missing or from another build.
Get-ChildItem "$src\BMP" -Filter 'Version_*.rsc' | Remove-Item
$sha = [Security.Cryptography.SHA256]::Create()
$files = Get-ChildItem $src -Recurse -File | Where-Object { $_.Name -notin 'icon.png', 'preview.jpg', 'bmpResources.rsc', 'Package.rsc' } |
    Sort-Object { $_.FullName.Substring($src.Length).ToLowerInvariant() }
$ms = New-Object IO.MemoryStream
foreach ($f in $files) {
    $n = [Text.Encoding]::UTF8.GetBytes($f.FullName.Substring($src.Length).ToLowerInvariant().Replace('\', '/'))
    $ms.Write($n, 0, $n.Length)
    $b = [IO.File]::ReadAllBytes($f.FullName)
    $ms.Write($b, 0, $b.Length)
}
$modVersion = ([BitConverter]::ToString($sha.ComputeHash($ms.ToArray())) -replace '-', '').Substring(0, 8).ToLowerInvariant()
@"
StringTable bmpVersion
{
	Entry _strings
	[
		{ String _name = "BmpModVersion";		String _text = "$modVersion"; }
	]
}
"@ | Set-Content "$src\BMP\Version_$modVersion.rsc" -Encoding ascii
"// generated by mod/make_mod_sources.ps1 - the companion mod build this DLL expects (BMP/Version_<this>.rsc)`n#define BMP_MOD_VERSION `"$modVersion`"" |
    Set-Content (Join-Path (Split-Path $PSScriptRoot) 'src\modversion.h') -Encoding ascii
"mod version $modVersion"

@'
ExternalList resource
{
	External _resources
	[
		"Template/StorageBarn.rsc"
		"Template/StorageYard.rsc"
		"Template/StorageCart.rsc"
		"Dialog/MainMenu.rsc"
		"Dialog/EventLog.rsc:content"
		"Dialog/Work.rsc:workPlace"
		"Dialog/BmpMultiplayer.rsc"
		"Dialog/BmpBar.rsc"
		"Dialog/BmpTrade.rsc"
		"Dialog/BmpOffer.rsc"
		"Dialog/BmpSettle.rsc"
		"Dialog/BmpReport.rsc"
		"Dialog/MpSprites.rsc"
		"BMP/ShareStrings.rsc:shareStrings"
		"BMP/Version_$modVersion.rsc:bmpVersion"
	]
}
'@.Replace('$modVersion', $modVersion) | Set-Content "$src\bmpResources.rsc" -Encoding ascii

@'
PackageFile bmpShare
{
	String _name = "Banished Multiplayer";
	String _author = "Banished Multiplayer";
	String _description = "Companion data mod for the Banished Multiplayer DLL: adds a Share button to storage barns,
		stockpiles and storage carts so another player's town can use them. Has no effect without the DLL.";
	String _icon = "icon.png";
	String _preview = "preview.jpg";
	int _userVersion = 1;

	String _includeList
	[
		"*"
	]

	String _excludeList
	[
		"Package_*.crs"
		"*.pkg"
		"*.pkm"
		"*.ps1"
		"Font\*"
		"Build\*"
	]
}
'@ | Set-Content "$src\Package.rsc" -Encoding ascii

# icon (48x48 png - the packager insists) and preview (jpg, shown in the Mods menu and on the Workshop): mod\assets.
# Not part of the mod version hash above, so new pictures don't make the DLL see "another build".
Copy-Item (Join-Path $PSScriptRoot 'assets\icon.png'), (Join-Path $PSScriptRoot 'assets\preview.jpg') $src -Force
"wrote BMP\ShareStrings.rsc, bmpResources.rsc, Package.rsc; copied icon.png, preview.jpg"
