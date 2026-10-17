// Banished Multiplayer: the in-game session bar (top centre). Texts and visibility come from the DLL (src/ingame.cpp).
Dialog resource
{
	ElementDescription _rootElement = "main";
	External _references
	[
		"BmpMultiplayer.rsc",
	]
}

GroupDescription main
{
	Alignment _alignment = TopCenter;
	ElementDescription _elements
	[
		"SharedElements.rsc:dialogBorder",
		"content"
	]
}

RibbonDescription content
{
	bool _vertical = true;
	int _leftPad = 6;
	int _rightPad = 6;
	int _topPad = 4;
	int _bottomPad = 4;
	int _cellPad = 3;
	ElementDescription _elements
	[
		"bmpBarRow",
		"bmpBarRoster",
		"bmpBarStatus",
		"bmpBarConfirm",
	]
}

RibbonDescription bmpBarRow
{
	bool _vertical = false;
	Alignment _alignment = MidLeft;
	int _cellPad = 4;
	ElementDescription _elements
	[
		"bmpBarState",
		"bmpBarInfo",
		"bmpBarTown0",
		"bmpBarTown1",
		"bmpBarTown2",
		"bmpBarTown3",
		"bmpBarColours",
		"bmpBarPlayers",
		"bmpBarInvite",
		"bmpBarTrade",
		"bmpBarResync",
		"bmpBarReport",
		"bmpBarLeave",
	]
}

TextDescription barText
{
	Alignment _alignment = MidLeft;
	int _leftPad = 2; int _rightPad = 2;
	Font _font = "Font\FontSmall.rsc";
	StringTable _stringTable = "StringTable.rsc:general";
	String _text = "Parameter";
	Color _color = 0xFFC0C0C0;
}
TextDescription bmpBarState : "barText" { Color _color = 0xFF8CD873; }
TextDescription bmpBarInfo : "barText" { }
TextDescription bmpBarTown0 : "barText" { }
TextDescription bmpBarTown1 : "barText" { }
TextDescription bmpBarTown2 : "barText" { }
TextDescription bmpBarTown3 : "barText" { }

TextDescription barButtonText : "barText" { int _leftPad = 6; int _rightPad = 6; int _topPad = 2; int _bottomPad = 2; Color _color = 0xFFFFFFFF; }
ButtonDescription barButton : "SharedElements.rsc:borderButton" { ElementDescription _content = "barButtonText"; }
ButtonDescription bmpBarPlayers : "barButton" { }
ButtonDescription bmpBarInvite : "barButton" { }
ButtonDescription bmpBarTrade : "barButton" { }
ButtonDescription bmpBarResync : "barButton" { }
ButtonDescription bmpBarLeave : "barButton" { }
ButtonDescription bmpBarReport : "barButton"
{
	ElementDescription _content = "bmpBarReportImg";
	Dialog _toolTipDialog = "Dialog/ToolTip.rsc";
	StringTable _toolTipStringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _toolTipText = "ReportTip";
}
ImageDescription bmpBarReportImg
{
	int _leftPad = 5; int _topPad = 1; int _rightPad = 5; int _bottomPad = 1;
	int _width = 16; int _height = 16;
	SpriteSheet _spriteSheet = "SpriteSheet.rsc";
	String _spriteName = "ToolDebug";
}

CheckDescription bmpBarColours : "SharedElements.rsc:checkSimple"
{
	ElementDescription _content = "barColoursOff";
	ElementDescription _check = "barColoursOn";
}
RibbonDescription barColoursOff
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 2;
	ElementDescription _elements [ "SharedElements.rsc:checkSimpleUnchecked", "barCheckText", ]
}
RibbonDescription barColoursOn
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 2;
	ElementDescription _elements [ "SharedElements.rsc:checkSimpleChecked", "barCheckText", ]
}
TextDescription barCheckText : "barText" { int _rightPad = 4; Color _color = 0xFFFFFFFF; }

TextDescription bmpBarRoster : "barText" { int _leftPad = 6; }
TextDescription bmpBarStatus : "barText"
{
	Color _color = 0xFFE8C060;
	bool _wordWrap = true;
	int _wordWrapSize = 560;
}

RibbonDescription bmpBarConfirm
{
	bool _vertical = false;
	Alignment _alignment = MidLeft;
	int _cellPad = 4;
	ElementDescription _elements [ "bmpBarConfirmText", "bmpBarYes", "bmpBarNo", ]
}
TextDescription bmpBarConfirmText : "barText" { Color _color = 0xFFFFFFFF; }
ButtonDescription bmpBarYes : "barButton" { }
ButtonDescription bmpBarNo : "barButton" { }
