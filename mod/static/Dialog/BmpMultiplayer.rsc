// Banished Multiplayer: the multiplayer window (main menu). Every text is a parameter the DLL fills in (src/mpdialog.cpp);
// the DLL finds the elements by name, numbers them and shows / hides the sections for the current state.
Dialog resource
{
	Flags _flags = Modal | FirstDraw;
	ElementDescription _rootElement = "main";

	External _references
	[
	]
}

GroupDescription main
{
	Alignment _alignment = MidCenter;
	ElementDescription _elements
	[
		"SharedElements.rsc:dialogBorder",
		"content"
	]
}

LayoutDescription content
{
	int _leftPad = 8;
	int _rightPad = 8;
	int _topPad = 8;
	int _bottomPad = 8;

	LayoutItem _elements
	[
		{
			String _name = "title";
			ElementDescription _element = "labelTitle";
			LayoutHorizontal _xLayout = LeftToLeft;
			LayoutVertical _yLayout = TopToTop; int _yOffset = 4;
		}
		{
			String _name = "body";
			ElementDescription _element = "body";
			LayoutHorizontal _xLayout = LeftToLeft; String _xAnchorTo = "title";
			LayoutVertical _yLayout = TopToBottom; String _yAnchorTo = "title";
		}
		{
			String _name = "close";
			ElementDescription _element = "bmpClose";
			LayoutHorizontal _xLayout = RightToRight; String _xAnchorTo = "body";
			LayoutVertical _yLayout = TopToBottom; String _yAnchorTo = "body"; int _yOffset = 4;
		}
		{
			String _name = "report";
			ElementDescription _element = "bmpReport";
			LayoutHorizontal _xLayout = RightToLeft; String _xAnchorTo = "close"; int _xOffset = -6;
			LayoutVertical _yLayout = MidToMid; String _yAnchorTo = "close";
		}
		{
			String _name = "version";
			ElementDescription _element = "bmpVersion";
			LayoutHorizontal _xLayout = LeftToLeft; String _xAnchorTo = "body";
			LayoutVertical _yLayout = MidToMid; String _yAnchorTo = "close";
		}
	]
}

LabelDescription labelTitle
{
	Alignment _alignment = MidCenter;
	int _bottomPad = 5;
	int _leftPad = 5;
	int _rightPad = 5;
	int _topPad = 5;
	Font _font = "Font\Font.rsc";
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	SpriteSheet _spriteSheet = "SpriteSheet.rsc";
	int _spacing = 4;
	LabelPosition _labelPosition = TextRight;
	Alignment _textAlignment = MidLeft;
	String _text = "Multiplayer";
	String _spriteName = "NomadTab";
}

RibbonDescription body
{
	ElementDescription _border = "SharedElements.rsc:sunkenRaisedBorder";
	bool _vertical = true;
	int _leftPad = 8;
	int _rightPad = 8;
	int _topPad = 8;
	int _bottomPad = 8;
	int _cellPad = 4;
	ElementDescription _elements
	[
		"bmpSetup",
		"bmpLobby",
		"bmpStatus",
	]
}

// ---------------------------------------------------------------- building blocks
TextDescription text
{
	Font _font = "Font\FontMedium.rsc";
	StringTable _stringTable = "StringTable.rsc:general";
	String _text = "Parameter";
	Color _color = 0xFFC0C0C0;
}
TextDescription textLabel : "text" { int _minWidth = 150; }
TextDescription textSmall
{
	Font _font = "Font\FontSmall.rsc";
	StringTable _stringTable = "StringTable.rsc:general";
	String _text = "Parameter";
	Color _color = 0xFFC0C0C0;
	bool _wordWrap = true;
	int _wordWrapSize = 440;
}
TextDescription textHeading : "text" { Color _color = 0xFFFFFFFF; int _topPad = 4; }

TextDescription buttonText : "SharedElements.rsc:commonButtonText"
{
	StringTable _stringTable = "StringTable.rsc:general";
	String _text = "Parameter";
}
ButtonDescription button : "SharedElements.rsc:borderButton" { ElementDescription _content = "buttonText"; }

CheckDescription check : "SharedElements.rsc:checkSimple"
{
	ElementDescription _content = "checkOff";
	ElementDescription _check = "checkOn";
}
RibbonDescription checkOff
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 2;
	ElementDescription _elements [ "SharedElements.rsc:checkSimpleUnchecked", "checkText", ]
}
RibbonDescription checkOn
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 2;
	ElementDescription _elements [ "SharedElements.rsc:checkSimpleChecked", "checkText", ]
}
TextDescription checkText
{
	Alignment _alignment = MidLeft;
	int _leftPad = 2; int _rightPad = 6; int _topPad = 3; int _bottomPad = 3;
	Font _font = "Font\FontMedium.rsc";
	StringTable _stringTable = "StringTable.rsc:general";
	String _text = "Parameter";
}

ComboDescription combo : "SharedElements.rsc:textCombo"
{
	Alignment _alignment = MidLeft;
	int _minWidth = 260;
}

EditDescription edit
{
	ElementDescription _border = "SharedElements.rsc:sunkenBorder";
	int _leftPad = 2; int _topPad = 2; int _rightPad = 2; int _bottomPad = 2;
	Font _font = "Font\FontMedium.rsc";
	int _maxLength = 60;
	int _minWidth = 260;
	String _allowableCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.:-_ ";
}
NumberEditDescription numberEdit
{
	ElementDescription _border = "SharedElements.rsc:sunkenBorder";
	int _leftPad = 2; int _topPad = 2; int _rightPad = 2; int _bottomPad = 2;
	Font _font = "Font\FontMedium.rsc";
	int _maxLength = 9;
	int _minValue = 0;
	int _maxValue = 999999999;
	int _minWidth = 260;
	bool _allowNegative = false;
}

// ---------------------------------------------------------------- setup (not in a game yet)
RibbonDescription bmpSetup
{
	bool _vertical = true;
	int _cellPad = 4;
	ElementDescription _elements
	[
		"bmpIntro",
		"bmpModeRow",
		"bmpSourceRow",
		"bmpNewTable",
		"bmpSaveRow",
		"bmpHint",
		"bmpHost",
		"bmpJoinHeading",
		"bmpJoinHint",
		"bmpJoinRow",
		"bmpConnHeading",
		"bmpConnRow",
		"bmpConnTable",
		"bmpConnHint",
	]
}
TextDescription bmpIntro : "text" { }
RibbonDescription bmpModeRow
{
	bool _vertical = false; int _cellPad = 8;
	ElementDescription _elements [ "bmpModeShare", "bmpModeSeparate", ]
}
CheckDescription bmpModeShare : "SharedElements.rsc:checkSimple"
{
	ElementDescription _content = "bmpModeShareOff";
	ElementDescription _check = "bmpModeShareOn";
}
RibbonDescription bmpModeShareOff
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "bmpModeShareImgOff", "checkText", ]
}
RibbonDescription bmpModeShareOn
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "bmpModeShareImg", "checkText", ]
}
ImageDescription bmpModeShareImg
{
	int _leftPad = 3; int _topPad = 3; int _rightPad = 0; int _bottomPad = 3;
	int _width = 32; int _height = 32;
	SpriteSheet _spriteSheet = "SpriteSheet.rsc";
	String _spriteName = "ToolbarHousing";
}
ImageDescription bmpModeShareImgOff : "bmpModeShareImg" { Color _color = 0xFF6A645C; }
CheckDescription bmpModeSeparate : "SharedElements.rsc:checkSimple"
{
	ElementDescription _content = "bmpModeSeparateOff";
	ElementDescription _check = "bmpModeSeparateOn";
}
RibbonDescription bmpModeSeparateOff
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "bmpModeSeparateImgOff", "checkText", ]
}
RibbonDescription bmpModeSeparateOn
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "bmpModeSeparateImg", "checkText", ]
}
ImageDescription bmpModeSeparateImg
{
	int _leftPad = 3; int _topPad = 3; int _rightPad = 0; int _bottomPad = 3;
	int _width = 32; int _height = 32;
	SpriteSheet _spriteSheet = "SpriteSheet.rsc";
	String _spriteName = "AchieveTown";
}
ImageDescription bmpModeSeparateImgOff : "bmpModeSeparateImg" { Color _color = 0xFF6A645C; }
RibbonDescription bmpSourceRow
{
	bool _vertical = false; int _cellPad = 8;
	ElementDescription _elements [ "bmpSourceNew", "bmpSourceSave", ]
}
CheckDescription bmpSourceNew : "SharedElements.rsc:checkSimple"
{
	ElementDescription _content = "bmpSourceNewOff";
	ElementDescription _check = "bmpSourceNewOn";
}
RibbonDescription bmpSourceNewOff
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "bmpSourceNewImgOff", "checkText", ]
}
RibbonDescription bmpSourceNewOn
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "bmpSourceNewImg", "checkText", ]
}
ImageDescription bmpSourceNewImg
{
	int _leftPad = 3; int _topPad = 3; int _rightPad = 0; int _bottomPad = 3;
	SpriteSheet _spriteSheet = "SpriteSheet.rsc";
	String _spriteName = "NewGame";
}
ImageDescription bmpSourceNewImgOff : "bmpSourceNewImg" { Color _color = 0xFF6A645C; }
CheckDescription bmpSourceSave : "SharedElements.rsc:checkSimple"
{
	ElementDescription _content = "bmpSourceSaveOff";
	ElementDescription _check = "bmpSourceSaveOn";
}
RibbonDescription bmpSourceSaveOff
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "bmpSourceSaveImgOff", "checkText", ]
}
RibbonDescription bmpSourceSaveOn
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 4;
	ElementDescription _elements [ "bmpSourceSaveImg", "checkText", ]
}
ImageDescription bmpSourceSaveImg
{
	int _leftPad = 3; int _topPad = 3; int _rightPad = 0; int _bottomPad = 3;
	SpriteSheet _spriteSheet = "SpriteSheet.rsc";
	String _spriteName = "LoadGame";
}
ImageDescription bmpSourceSaveImgOff : "bmpSourceSaveImg" { Color _color = 0xFF6A645C; }

TableDescription bmpNewTable
{
	int _width = 2;
	int _height = 6;
	int _cellPad = 4;
	TableItem _items
	[
		{ int _x = 0; int _y = 0; ElementDescription _element = "bmpStartLabel"; }
		{ int _x = 0; int _y = 1; ElementDescription _element = "bmpSizeLabel"; }
		{ int _x = 0; int _y = 2; ElementDescription _element = "bmpTerrainLabel"; }
		{ int _x = 0; int _y = 3; ElementDescription _element = "bmpClimateLabel"; }
		{ int _x = 0; int _y = 4; ElementDescription _element = "bmpSeedLabel"; }
		{ int _x = 1; int _y = 0; ElementDescription _element = "bmpStart"; }
		{ int _x = 1; int _y = 1; ElementDescription _element = "bmpSize"; }
		{ int _x = 1; int _y = 2; ElementDescription _element = "bmpTerrain"; }
		{ int _x = 1; int _y = 3; ElementDescription _element = "bmpClimate"; }
		{ int _x = 1; int _y = 4; ElementDescription _element = "bmpSeed"; }
		{ int _x = 1; int _y = 5; ElementDescription _element = "bmpPick"; }
	]
}
TextDescription bmpStartLabel : "textLabel" { }
TextDescription bmpSizeLabel : "textLabel" { }
TextDescription bmpTerrainLabel : "textLabel" { }
TextDescription bmpClimateLabel : "textLabel" { }
TextDescription bmpSeedLabel : "textLabel" { }
ComboDescription bmpStart : "combo" { }
ComboDescription bmpSize : "combo" { }
ComboDescription bmpTerrain : "combo" { }
ComboDescription bmpClimate : "combo" { }
NumberEditDescription bmpSeed : "numberEdit" { }
CheckDescription bmpPick : "check" { }

RibbonDescription bmpSaveRow
{
	bool _vertical = false; int _cellPad = 4;
	ElementDescription _elements [ "bmpSaveLabel", "bmpSave", ]
}
TextDescription bmpSaveLabel : "textLabel" { }
ComboDescription bmpSave : "combo" { }
TextDescription bmpHint : "textSmall" { int _minHeight = 48; }
ButtonDescription bmpHost : "button" { }

TextDescription bmpJoinHeading : "textHeading" { }
TextDescription bmpJoinHint : "textSmall" { int _minHeight = 32; }
RibbonDescription bmpJoinRow
{
	bool _vertical = false; int _cellPad = 4;
	ElementDescription _elements [ "bmpJoinEdit", "bmpPaste", "bmpJoin", ]
}
EditDescription bmpJoinEdit : "edit" { int _minWidth = 260; }
ButtonDescription bmpPaste : "SharedElements.rsc:borderButton"
{
	ElementDescription _content = "bmpPasteImg";
	Dialog _toolTipDialog = "Dialog/ToolTip.rsc";
	StringTable _toolTipStringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _toolTipText = "PasteTip";
}
ImageDescription bmpPasteImg
{
	int _leftPad = 4; int _topPad = 2; int _rightPad = 4; int _bottomPad = 2;
	int _width = 24; int _height = 24;
	SpriteSheet _spriteSheet = "MpSprites.rsc";
	String _spriteName = "BmpPaste";
}
ButtonDescription bmpJoin : "button" { }

TextDescription bmpConnHeading : "textHeading" { }
RibbonDescription bmpConnRow
{
	bool _vertical = false; int _cellPad = 8;
	ElementDescription _elements [ "bmpConnSteam", "bmpConnDirect", "bmpConnRelay", ]
}
CheckDescription bmpConnSteam : "check" { }
CheckDescription bmpConnDirect : "check" { }
CheckDescription bmpConnRelay : "check" { }
TableDescription bmpConnTable
{
	int _width = 2;
	int _height = 3;
	int _cellPad = 4;
	TableItem _items
	[
		{ int _x = 0; int _y = 0; ElementDescription _element = "bmpPortLabel"; }
		{ int _x = 0; int _y = 1; ElementDescription _element = "bmpRelayLabel"; }
		{ int _x = 0; int _y = 2; ElementDescription _element = "bmpNameLabel"; }
		{ int _x = 1; int _y = 0; ElementDescription _element = "bmpPort"; }
		{ int _x = 1; int _y = 1; ElementDescription _element = "bmpRelay"; }
		{ int _x = 1; int _y = 2; ElementDescription _element = "bmpName"; }
	]
}
TextDescription bmpPortLabel : "textLabel" { }
TextDescription bmpRelayLabel : "textLabel" { }
TextDescription bmpNameLabel : "textLabel" { }
NumberEditDescription bmpPort : "numberEdit" { int _maxLength = 5; int _minValue = 1; int _maxValue = 65535; }
EditDescription bmpRelay : "edit" { }
EditDescription bmpName : "edit" { int _maxLength = 24; }
TextDescription bmpConnHint : "textSmall" { int _minHeight = 32; }

// ---------------------------------------------------------------- lobby (hosting / joined, not started)
RibbonDescription bmpLobby
{
	bool _vertical = true;
	int _cellPad = 4;
	ElementDescription _elements
	[
		"bmpLobbyTitle",
		"bmpRoster",
		"bmpLobbyInfo",
		"bmpLobbyButtons",
	]
}
TextDescription bmpLobbyTitle : "textHeading" { }
TextDescription bmpRoster : "text" { int _leftPad = 8; int _minWidth = 440; }
TextDescription bmpLobbyInfo : "textSmall" { }
RibbonDescription bmpLobbyButtons
{
	bool _vertical = false; int _cellPad = 4;
	ElementDescription _elements [ "bmpInvite", "bmpCopy", "bmpStartGame", "bmpLeave", ]
}
ButtonDescription bmpInvite : "button" { }
ButtonDescription bmpCopy : "SharedElements.rsc:borderButton"
{
	ElementDescription _content = "bmpCopyImg";
	Dialog _toolTipDialog = "Dialog/ToolTip.rsc";
	StringTable _toolTipStringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _toolTipText = "CopyTip";
}
ImageDescription bmpCopyImg
{
	int _leftPad = 4; int _topPad = 2; int _rightPad = 4; int _bottomPad = 2;
	int _width = 24; int _height = 24;
	SpriteSheet _spriteSheet = "MpSprites.rsc";
	String _spriteName = "BmpCopy";
}
ButtonDescription bmpStartGame : "button" { }
ButtonDescription bmpLeave : "button" { }

// ---------------------------------------------------------------- always
TextDescription bmpStatus : "textSmall" { Color _color = 0xFFE8C060; }
TextDescription bmpVersion
{
	Font _font = "Font\FontSmall.rsc";
	StringTable _stringTable = "StringTable.rsc:general";
	String _text = "Parameter";
	Color _color = 0xFF908880;
}
ButtonDescription bmpClose : "button" { }
ButtonDescription bmpReport : "SharedElements.rsc:borderButton"
{
	ElementDescription _content = "bmpReportImg";
	Dialog _toolTipDialog = "Dialog/ToolTip.rsc";
	StringTable _toolTipStringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _toolTipText = "ReportTip";
}
ImageDescription bmpReportImg
{
	int _leftPad = 6; int _topPad = 2; int _rightPad = 6; int _bottomPad = 2;
	int _width = 20; int _height = 20;
	SpriteSheet _spriteSheet = "SpriteSheet.rsc";
	String _spriteName = "ToolDebug";
}
