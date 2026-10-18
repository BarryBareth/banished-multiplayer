// Banished Multiplayer: the mod's own icons (drawn by the user, mod/static/Dialog/Sprite/Bmp; originals in icons/).
// Same layout as the game's Dialog/SpriteSheet.rsc: the kit packs the sources into Build/MpSprites.png.
SpriteSheet resource
{
	String _materialName = "MpSpritesMaterial.rsc";
	String _imageName = "Build/MpSprites.png";
	int _imageWidth = 256;
	int _imageHeight = 64;
	bool _padForFiltering = true;

	Sprite _sprites
	[
		{ String _name = "BmpCopy";			String _source = "Sprite/Bmp/Copy.png"; }
		{ String _name = "BmpPaste";		String _source = "Sprite/Bmp/Paste.png"; }
	]
}
