// Banished Multiplayer: trading between towns (opened from the session bar). Driven by src/ingame.cpp.
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
	int _topPad = 64;
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
	int _topPad = 6;
	int _bottomPad = 8;
	LayoutItem _elements
	[
		{
			String _name = "title";
			ElementDescription _element = "titleTrade";
			LayoutHorizontal _xLayout = LeftToLeft;
			LayoutVertical _yLayout = TopToTop;
		}
		{
			String _name = "body";
			ElementDescription _element = "body";
			LayoutHorizontal _xLayout = LeftToLeft; String _xAnchorTo = "title";
			LayoutVertical _yLayout = TopToBottom; String _yAnchorTo = "title"; int _yOffset = 4;
		}
		{
			String _name = "close";
			ElementDescription _element = "bmpTradeClose";
			LayoutHorizontal _xLayout = RightToRight; String _xAnchorTo = "body";
			LayoutVertical _yLayout = MidToMid; String _yAnchorTo = "title";
		}
	]
}

LabelDescription titleTrade : "BmpMultiplayer.rsc:labelTitle"
{
	Font _font = "Font\FontMedium.rsc";
	StringTable _stringTable = "BMP/ShareStrings.rsc:shareStrings";
	String _text = "TradeTitle";
	String _spriteName = "TradeTab";
}
ButtonDescription bmpTradeClose : "StandardDialog.rsc:buttonClose" { }

RibbonDescription body
{
	ElementDescription _border = "SharedElements.rsc:sunkenRaisedBorder";
	bool _vertical = true;
	int _leftPad = 8; int _rightPad = 8; int _topPad = 8; int _bottomPad = 8;
	int _cellPad = 4;
	ElementDescription _elements
	[
		"bmpNobody",
		"bmpPartnerRow",
		"bmpOfferTable",
		"bmpPeriodRow",
		"bmpSend",
		"bmpTradeHint",
		"bmpNews",
		"bmpStandingHeading",
		"bmpAgree0",
		"bmpAgree1",
		"bmpAgree2",
		"bmpAgree3",
		"bmpAgree4",
		"bmpAgree5",
		"bmpWaitHeading",
		"bmpWaiting",
		"bmpDeliver",
	]
}

TextDescription bmpNobody : "BmpMultiplayer.rsc:text" { }
RibbonDescription bmpPartnerRow
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 6;
	ElementDescription _elements [ "bmpPartnerLabel", "bmpPartner0", "bmpPartner1", "bmpPartner2", "bmpPartner3", ]
}
TextDescription bmpPartnerLabel : "BmpMultiplayer.rsc:text" { int _minWidth = 100; }
CheckDescription bmpPartner0 : "BmpMultiplayer.rsc:check" { }
CheckDescription bmpPartner1 : "BmpMultiplayer.rsc:check" { }
CheckDescription bmpPartner2 : "BmpMultiplayer.rsc:check" { }
CheckDescription bmpPartner3 : "BmpMultiplayer.rsc:check" { }

TableDescription bmpOfferTable
{
	int _width = 3;
	int _height = 2;
	int _cellPad = 4;
	TableItem _items
	[
		{ int _x = 0; int _y = 0; ElementDescription _element = "bmpGiveLabel"; }
		{ int _x = 0; int _y = 1; ElementDescription _element = "bmpAskLabel"; }
		{ int _x = 1; int _y = 0; ElementDescription _element = "bmpGive"; }
		{ int _x = 1; int _y = 1; ElementDescription _element = "bmpAsk"; }
		{ int _x = 2; int _y = 0; ElementDescription _element = "bmpGiveN"; }
		{ int _x = 2; int _y = 1; ElementDescription _element = "bmpAskN"; }
	]
}
TextDescription bmpGiveLabel : "BmpMultiplayer.rsc:text" { int _minWidth = 100; }
TextDescription bmpAskLabel : "BmpMultiplayer.rsc:text" { int _minWidth = 100; }
ComboDescription bmpGive : "BmpMultiplayer.rsc:combo" { }
ComboDescription bmpAsk : "BmpMultiplayer.rsc:combo" { }
NumberEditDescription bmpGiveN : "BmpMultiplayer.rsc:numberEdit" { int _minWidth = 90; int _maxLength = 5; int _maxValue = 99999; }
NumberEditDescription bmpAskN : "BmpMultiplayer.rsc:numberEdit" { int _minWidth = 90; int _maxLength = 5; int _maxValue = 99999; }

RibbonDescription bmpPeriodRow
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 6;
	ElementDescription _elements [ "bmpPeriodLabel", "bmpOnce", "bmpSeason", "bmpYear", ]
}
TextDescription bmpPeriodLabel : "BmpMultiplayer.rsc:text" { int _minWidth = 100; }
CheckDescription bmpOnce : "BmpMultiplayer.rsc:check" { }
CheckDescription bmpSeason : "BmpMultiplayer.rsc:check" { }
CheckDescription bmpYear : "BmpMultiplayer.rsc:check" { }

ButtonDescription bmpSend : "BmpMultiplayer.rsc:button" { }
TextDescription bmpTradeHint : "BmpMultiplayer.rsc:textSmall" { }
TextDescription bmpNews : "BmpMultiplayer.rsc:textSmall" { Color _color = 0xFFE8C060; }

TextDescription bmpStandingHeading : "BmpMultiplayer.rsc:textHeading" { }
RibbonDescription agreeRow
{
	bool _vertical = false; Alignment _alignment = MidLeft; int _cellPad = 6;
}
TextDescription agreeText : "BmpMultiplayer.rsc:textSmall" { int _wordWrapSize = 400; }
ButtonDescription agreeCancel : "BmpMultiplayer.rsc:button" { }
RibbonDescription bmpAgree0 : "agreeRow" { ElementDescription _elements [ "bmpAgreeText0", "bmpAgreeCancel0", ] }
RibbonDescription bmpAgree1 : "agreeRow" { ElementDescription _elements [ "bmpAgreeText1", "bmpAgreeCancel1", ] }
RibbonDescription bmpAgree2 : "agreeRow" { ElementDescription _elements [ "bmpAgreeText2", "bmpAgreeCancel2", ] }
RibbonDescription bmpAgree3 : "agreeRow" { ElementDescription _elements [ "bmpAgreeText3", "bmpAgreeCancel3", ] }
RibbonDescription bmpAgree4 : "agreeRow" { ElementDescription _elements [ "bmpAgreeText4", "bmpAgreeCancel4", ] }
RibbonDescription bmpAgree5 : "agreeRow" { ElementDescription _elements [ "bmpAgreeText5", "bmpAgreeCancel5", ] }
TextDescription bmpAgreeText0 : "agreeText" { }
TextDescription bmpAgreeText1 : "agreeText" { }
TextDescription bmpAgreeText2 : "agreeText" { }
TextDescription bmpAgreeText3 : "agreeText" { }
TextDescription bmpAgreeText4 : "agreeText" { }
TextDescription bmpAgreeText5 : "agreeText" { }
ButtonDescription bmpAgreeCancel0 : "agreeCancel" { }
ButtonDescription bmpAgreeCancel1 : "agreeCancel" { }
ButtonDescription bmpAgreeCancel2 : "agreeCancel" { }
ButtonDescription bmpAgreeCancel3 : "agreeCancel" { }
ButtonDescription bmpAgreeCancel4 : "agreeCancel" { }
ButtonDescription bmpAgreeCancel5 : "agreeCancel" { }

TextDescription bmpWaitHeading : "BmpMultiplayer.rsc:textHeading" { }
TextDescription bmpWaiting : "BmpMultiplayer.rsc:textSmall" { }
ButtonDescription bmpDeliver : "BmpMultiplayer.rsc:button" { }
