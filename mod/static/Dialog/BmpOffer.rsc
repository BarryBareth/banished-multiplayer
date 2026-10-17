// Banished Multiplayer: a trade offer from another player (top right). Driven by src/ingame.cpp.
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
	Alignment _alignment = TopRight;
	int _topPad = 120;
	int _rightPad = 20;
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
		"titleOffer",
		"bmpOfferText",
		"bmpOfferHave",
		"bmpOfferButtons",
	]
}
LabelDescription titleOffer : "BmpMultiplayer.rsc:labelTitle"
{
	Font _font = "Font\FontMedium.rsc";
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _text = "OfferTitle";
	String _spriteName = "TradeTab";
}
TextDescription bmpOfferText : "BmpMultiplayer.rsc:text" { bool _wordWrap = true; int _wordWrapSize = 340; }
TextDescription bmpOfferHave : "BmpMultiplayer.rsc:textSmall" { int _wordWrapSize = 340; }
RibbonDescription bmpOfferButtons
{
	bool _vertical = false; int _cellPad = 4;
	ElementDescription _elements [ "bmpAccept", "bmpDecline", ]
}
ButtonDescription bmpAccept : "BmpMultiplayer.rsc:button" { }
ButtonDescription bmpDecline : "BmpMultiplayer.rsc:button" { }
