// Banished Multiplayer: bug report (opened from the Multiplayer window, the in-game bar, or after a crash).
// Driven by src/report.cpp.
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
	Alignment _alignment = MidCenter;
	ElementDescription _elements
	[
		"SharedElements.rsc:dialogBorder",
		"content"
	]
}

RibbonDescription content
{
	bool _vertical = true;
	int _leftPad = 12; int _rightPad = 12; int _topPad = 10; int _bottomPad = 10;
	int _cellPad = 6;
	ElementDescription _elements
	[
		"bmpReportTitle",
		"bmpReportIntro",
		"bmpReportEdit",
		"bmpReportWhat",
		"bmpReportStatus",
		"bmpReportButtons",
	]
}
TextDescription bmpReportTitle : "BmpMultiplayer.rsc:textHeading" { Font _font = "Font\Font.rsc"; }
TextDescription bmpReportIntro : "BmpMultiplayer.rsc:text" { bool _wordWrap = true; int _wordWrapSize = 560; }
EditDescription bmpReportEdit : "BmpMultiplayer.rsc:edit"
{
	int _maxLength = 300;
	int _minWidth = 560;
	String _allowableCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,!?'()-:;/+&%#=";
}
TextDescription bmpReportWhat : "BmpMultiplayer.rsc:textSmall" { bool _wordWrap = true; int _wordWrapSize = 560; }
TextDescription bmpReportStatus : "BmpMultiplayer.rsc:textSmall" { Color _color = 0xFFE8C060; bool _wordWrap = true; int _wordWrapSize = 560; }

RibbonDescription bmpReportButtons
{
	bool _vertical = false;
	Alignment _alignment = MidRight;
	int _cellPad = 6;
	ElementDescription _elements [ "bmpReportSave", "bmpReportSend", "bmpReportClose", ]
}
ButtonDescription bmpReportSave : "BmpMultiplayer.rsc:button" { }
ButtonDescription bmpReportSend : "BmpMultiplayer.rsc:button" { }
ButtonDescription bmpReportClose : "BmpMultiplayer.rsc:button" { }
