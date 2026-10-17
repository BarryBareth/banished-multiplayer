// Banished Multiplayer: choosing where your town starts (bottom centre). Driven by src/ingame.cpp.
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
	Alignment _alignment = BottomCenter;
	int _bottomPad = 60;
	ElementDescription _elements
	[
		"SharedElements.rsc:dialogBorder",
		"content"
	]
}

RibbonDescription content
{
	bool _vertical = true;
	int _leftPad = 10; int _rightPad = 10; int _topPad = 8; int _bottomPad = 8;
	int _cellPad = 4;
	ElementDescription _elements
	[
		"titleSettle",
		"bmpSettleText",
		"bmpSettleCell",
		"bmpSettleWhy",
		"bmpSettle",
	]
}
LabelDescription titleSettle : "BmpMultiplayer.rsc:labelTitle"
{
	Font _font = "Font\FontMedium.rsc";
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _text = "SettleTitle";
	String _spriteName = "BuildTownHall";
}
TextDescription bmpSettleText : "BmpMultiplayer.rsc:text" { }
TextDescription bmpSettleCell : "BmpMultiplayer.rsc:textSmall" { }
TextDescription bmpSettleWhy : "BmpMultiplayer.rsc:text" { }
ButtonDescription bmpSettle : "BmpMultiplayer.rsc:button" { }
