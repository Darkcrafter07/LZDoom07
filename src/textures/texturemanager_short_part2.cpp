//texturemanager_short.cpp - part 2 start
void FTextureManager::AddPatches (int lumpnum)
{
	auto file = Wads.ReopenLumpReader (lumpnum, true);
	uint32_t numpatches, i;
	char name[9];

	numpatches = file.ReadUInt32();
	name[8] = '\0';

	for (i = 0; i < numpatches; ++i)
	{
		file.Read (name, 8);

		if (CheckForTexture (name, ETextureType::WallPatch, 0) == -1)
		{
			CreateTexture (Wads.CheckNumForName (name, ns_patches), ETextureType::WallPatch);
		}
		StartScreen->Progress();
	}
}

void FTextureManager::LoadTextureX(int wadnum)
{
	// Use the most recent PNAMES for this WAD.
	// Multiple PNAMES in a WAD will be ignored.
	int pnames = Wads.CheckNumForName("PNAMES", ns_global, wadnum, false);

	if (pnames < 0)
	{
		// should never happen except for zdoom.pk3
		return;
	}

	// Only add the patches if the PNAMES come from the current file
	// Otherwise they have already been processed.
	if (Wads.GetLumpFile(pnames) == wadnum) TexMan.AddPatches (pnames);

	int texlump1 = Wads.CheckNumForName ("TEXTURE1", ns_global, wadnum);
	int texlump2 = Wads.CheckNumForName ("TEXTURE2", ns_global, wadnum);
	AddTexturesLumps (texlump1, texlump2, pnames);
}

void FTextureManager::AddTexturesForWad(int wadnum)
{
	int firsttexture = Textures.Size();
	int lumpcount = Wads.GetNumLumps();

	FirstTextureForFile.Push(firsttexture);

	// First step: Load sprites
	AddGroup(wadnum, ns_sprites, ETextureType::Sprite);

	// When loading a Zip, all graphics in the patches/ directory should be
	// added as well.
	AddGroup(wadnum, ns_patches, ETextureType::WallPatch);

	// Second step: TEXTUREx lumps
	LoadTextureX(wadnum);

	// Third step: Flats
	AddGroup(wadnum, ns_flats, ETextureType::Flat);

	// Fourth step: Textures (TX_)
	AddGroup(wadnum, ns_newtextures, ETextureType::Override);

	// Sixth step: Try to find any lump in the WAD that may be a texture and load as a TEX_MiscPatch
	int firsttx = Wads.GetFirstLump(wadnum);
	int lasttx = Wads.GetLastLump(wadnum);

	for (int i= firsttx; i <= lasttx; i++)
	{
		bool skin = false;
		FString Name;
		Wads.GetLumpName(Name, i);

		// Ignore anything not in the global namespace
		int ns = Wads.GetLumpNamespace(i);
		if (ns == ns_global)
		{
			// In Zips all graphics must be in a separate namespace.
			if (Wads.GetLumpFlags(i) & LUMPF_ZIPFILE) continue;

			// Ignore lumps with empty names.
			if (Wads.CheckLumpName(i, "")) continue;

			// Ignore anything belonging to a map
			if (Wads.CheckLumpName(i, "THINGS")) continue;
			if (Wads.CheckLumpName(i, "LINEDEFS")) continue;
			if (Wads.CheckLumpName(i, "SIDEDEFS")) continue;
			if (Wads.CheckLumpName(i, "VERTEXES")) continue;
			if (Wads.CheckLumpName(i, "SEGS")) continue;
			if (Wads.CheckLumpName(i, "SSECTORS")) continue;
			if (Wads.CheckLumpName(i, "NODES")) continue;
			if (Wads.CheckLumpName(i, "SECTORS")) continue;
			if (Wads.CheckLumpName(i, "REJECT")) continue;
			if (Wads.CheckLumpName(i, "BLOCKMAP")) continue;
			if (Wads.CheckLumpName(i, "BEHAVIOR")) continue;

			// Don't bother looking at this lump if something later overrides it.
			if (Wads.CheckNumForName(Name, ns_graphics) != i) continue;

			// skip this if it has already been added as a wall patch.
			if (CheckForTexture(Name, ETextureType::WallPatch, 0).Exists()) continue;
		}
		else if (ns == ns_graphics)
		{
			// Don't bother looking this lump if something later overrides it.
			if (Wads.CheckNumForName(Name, ns_graphics) != i) continue;
		}
		else if (ns >= ns_firstskin)
		{
			// Don't bother looking this lump if something later overrides it.
			if (Wads.CheckNumForName(Name, ns) != i) continue;
			skin = true;
		}
		else continue;

		// Try to create a texture from this lump and add it.
		// Unfortunately we have to look at everything that comes through here...
		FTexture *out = FTexture::CreateTexture(i, skin ? ETextureType::SkinGraphic : ETextureType::MiscPatch);

		if (out != NULL) 
		{
			AddTexture (out);
		}
	}

	// Check for text based texture definitions
	LoadTextureDefs(wadnum, "TEXTURES");
	LoadTextureDefs(wadnum, "HIRESTEX");

	// Seventh step: Check for hires replacements.
	AddHiresTextures(wadnum);

	SortTexturesByType(firsttexture, Textures.Size());
}

void FTextureManager::SortTexturesByType(int start, int end)
{
	TArray<FTexture *> newtextures;

	// First unlink all newly added textures from the hash chain
	for (int i = 0; i < HASH_SIZE; i++)
	{
		while (HashFirst[i] >= start && HashFirst[i] != HASH_END)
		{
			HashFirst[i] = Textures[HashFirst[i]].HashNext;
		}
	}
	newtextures.Resize(end-start);
	for(int i=start; i<end; i++)
	{
		newtextures[i-start] = Textures[i].Texture;
	}
	Textures.Resize(start);
	Translation.Resize(start);

	static ETextureType texturetypes[] = {
		ETextureType::Sprite, ETextureType::Null, ETextureType::FirstDefined, 
		ETextureType::WallPatch, ETextureType::Wall, ETextureType::Flat, 
		ETextureType::Override, ETextureType::MiscPatch, ETextureType::SkinGraphic
	};

	for(unsigned int i=0;i<countof(texturetypes);i++)
	{
		for(unsigned j = 0; j<newtextures.Size(); j++)
		{
			if (newtextures[j] != NULL && newtextures[j]->UseType == texturetypes[i])
			{
				AddTexture(newtextures[j]);
				newtextures[j] = NULL;
			}
		}
	}
	// This should never happen. All other UseTypes are only used outside
	for(unsigned j = 0; j<newtextures.Size(); j++)
	{
		if (newtextures[j] != NULL)
		{
			Printf("Texture %s has unknown type!\n", newtextures[j]->Name.GetChars());
			AddTexture(newtextures[j]);
		}
	}
}

FTexture *GetBackdropTexture();
FTexture *CreateShaderTexture(bool, bool);

void FTextureManager::Init()
{
	DeleteAll();
	SpriteFrames.Clear();
	//if (BuildTileFiles.Size() == 0) CountBuildTiles ();
	FTexture::InitGrayMap();

	// Texture 0 is a dummy texture used to indicate "no texture"
	AddTexture (new FDummyTexture);
	// some special textures used in the game.
	AddTexture(CreateShaderTexture(false, false));
	AddTexture(CreateShaderTexture(false, true));
	AddTexture(CreateShaderTexture(true, false));
	AddTexture(CreateShaderTexture(true, true));

	int wadcnt = Wads.GetNumWads();
	for(int i = 0; i< wadcnt; i++)
	{
		AddTexturesForWad(i);
	}
	for (unsigned i = 0; i < Textures.Size(); i++)
	{
		Textures[i].Texture->ResolvePatches();
	}

	// Add one marker so that the last WAD is easier to handle and treat
	// Build tiles as a completely separate block.
	FirstTextureForFile.Push(Textures.Size());
	InitBuildTiles ();
	FirstTextureForFile.Push(Textures.Size());

	DefaultTexture = CheckForTexture ("-NOFLAT-", ETextureType::Override, 0);

	// The Hexen scripts use BLANK as a blank texture, even though it's really not.
	// I guess the Doom renderer must have clipped away the line at the bottom of
	// the texture so it wasn't visible. I'll just map it to 0, so it really is blank.
	if (gameinfo.gametype == GAME_Hexen)
	{
		FTextureID tex = CheckForTexture ("BLANK", ETextureType::Wall, false);
		if (tex.Exists())
		{
			SetTranslation (tex, 0);
		}
	}

	// Hexen parallax skies use color 0 to indicate transparency on the front
	// layer, so we must not remap color 0 on these textures. Unfortunately,
	// the only way to identify these textures is to check the MAPINFO.
	for (unsigned int i = 0; i < wadlevelinfos.Size(); ++i)
	{
		if (wadlevelinfos[i].flags & LEVEL_DOUBLESKY)
		{
			FTextureID picnum = CheckForTexture (wadlevelinfos[i].SkyPic1, ETextureType::Wall, false);
			if (picnum.isValid())
			{
				Textures[picnum.GetIndex()].Texture->SetFrontSkyLayer ();
			}
		}
	}

	InitAnimated();
	InitAnimDefs();
	FixAnimations();
	InitSwitchList();
	InitPalettedVersions();
}

void FTextureManager::InitPalettedVersions()
{
	int lump, lastlump = 0;

	PalettedVersions.Clear();
	while ((lump = Wads.FindLump("PALVERS", &lastlump)) != -1)
	{
		FScanner sc(lump);

		while (sc.GetString())
		{
			FTextureID pic1 = CheckForTexture(sc.String, ETextureType::Any);
			if (!pic1.isValid())
			{
				sc.ScriptMessage("Unknown texture %s to replace", sc.String);
			}
			sc.MustGetString();
			FTextureID pic2 = CheckForTexture(sc.String, ETextureType::Any);
			if (!pic2.isValid())
			{
				sc.ScriptMessage("Unknown texture %s to use as replacement", sc.String);
			}
			if (pic1.isValid() && pic2.isValid())
			{
				PalettedVersions[pic1.GetIndex()] = pic2.GetIndex();
			}
		}
	}
}

FTextureID FTextureManager::PalCheck(FTextureID tex)
{
	if (vid_nopalsubstitutions) return tex;
	int *newtex = PalettedVersions.CheckKey(tex.GetIndex());
	if (newtex == NULL || *newtex == 0) return tex;
	return *newtex;
}

int FTextureManager::GuesstimateNumTextures ()
{
	int numtex = 0;
	
	for(int i = Wads.GetNumLumps()-1; i>=0; i--)
	{
		int space = Wads.GetLumpNamespace(i);
		switch(space)
		{
		case ns_flats:
		case ns_sprites:
		case ns_newtextures:
		case ns_hires:
		case ns_patches:
		case ns_graphics:
			numtex++;
			break;

		default:
			if (Wads.GetLumpFlags(i) & LUMPF_MAYBEFLAT) numtex++;

			break;
		}
	}

	//numtex += CountBuildTiles (); // this cannot be done with a lot of overhead so just leave it out.
	numtex += CountTexturesX ();
	return numtex;
}

int FTextureManager::CountTexturesX ()
{
	int count = 0;
	int wadcount = Wads.GetNumWads();
	for (int wadnum = 0; wadnum < wadcount; wadnum++)
	{
		// Use the most recent PNAMES for this WAD.
		// Multiple PNAMES in a WAD will be ignored.
		int pnames = Wads.CheckNumForName("PNAMES", ns_global, wadnum, false);

		// should never happen except for zdoom.pk3
		if (pnames < 0) continue;

		// Only count the patches if the PNAMES come from the current file
		// Otherwise they have already been counted.
		if (Wads.GetLumpFile(pnames) == wadnum) 
		{
			count += CountLumpTextures (pnames);
		}

		int texlump1 = Wads.CheckNumForName ("TEXTURE1", ns_global, wadnum);
		int texlump2 = Wads.CheckNumForName ("TEXTURE2", ns_global, wadnum);

		count += CountLumpTextures (texlump1) - 1;
		count += CountLumpTextures (texlump2) - 1;
	}
	return count;
}

int FTextureManager::CountLumpTextures (int lumpnum)
{
	if (lumpnum >= 0)
	{
		auto file = Wads.OpenLumpReader (lumpnum); 
		uint32_t numtex = file.ReadUInt32();;

		return int(numtex) >= 0 ? numtex : 0;
	}
	return 0;
}


DEFINE_ACTION_FUNCTION(_TexMan, GetName)
{
	PARAM_PROLOGUE;
	PARAM_INT(texid);
	auto tex = TexMan.ByIndex(texid);
	FString retval;

	if (tex != nullptr)
	{
		if (tex->Name.IsNotEmpty()) retval = tex->Name;
		else
		{
			// Textures for full path names do not have their own name, they merely link to the source lump.
			auto lump = tex->GetSourceLump();
			if (Wads.GetLinkedTexture(lump) == tex)
				retval = Wads.GetLumpFullName(lump);
		}
	}
	ACTION_RETURN_STRING(retval);
}


static int GetTextureSize(int texid, int *py)
{
	auto tex = TexMan.ByIndex(texid);
	int x, y;
	if (tex != nullptr)
	{
		x = tex->GetWidth();
		y = tex->GetHeight();
	}
	else x = y = -1;
	if (py) *py = y;
	return x;
}

DEFINE_ACTION_FUNCTION_NATIVE(_TexMan, GetSize, GetTextureSize)
{
	PARAM_PROLOGUE;
	PARAM_INT(texid);
	int x, y;
	x = GetTextureSize(texid, &y);
	if (numret > 0) ret[0].SetInt(x);
	if (numret > 1) ret[1].SetInt(y);
	return MIN(numret, 2);
}

static void GetScaledSize(int texid, DVector2 *pvec)
{
	auto tex = TexMan.ByIndex(texid);
	double x, y;
	if (tex != nullptr)
	{
		x = tex->GetScaledWidthDouble();
		y = tex->GetScaledHeightDouble();
	}
	else x = y = -1;
	if (pvec)
	{
		pvec->X = x;
		pvec->Y = y;
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_TexMan, GetScaledSize, GetScaledSize)
{
	PARAM_PROLOGUE;
	PARAM_INT(texid);
	DVector2 vec;
	GetScaledSize(texid, &vec);
	ACTION_RETURN_VEC2(vec);
}

static void GetScaledOffset(int texid, DVector2 *pvec)
{
	auto tex = TexMan.ByIndex(texid);
	double x, y;
	if (tex != nullptr)
	{
		x = tex->GetScaledLeftOffsetDouble();
		y = tex->GetScaledTopOffsetDouble();
	}
	else x = y = -1;
	if (pvec)
	{
		pvec->X = x;
		pvec->Y = y;
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(_TexMan, GetScaledOffset, GetScaledOffset)
{
	PARAM_PROLOGUE;
	PARAM_INT(texid);
	DVector2 vec;
	GetScaledOffset(texid, &vec);
	ACTION_RETURN_VEC2(vec);
}


static int CheckRealHeight(int texid)
{
	auto tex = TexMan.ByIndex(texid);
	if (tex != nullptr) return tex->CheckRealHeight();
	else return -1;
}

DEFINE_ACTION_FUNCTION_NATIVE(_TexMan, CheckRealHeight, CheckRealHeight)
{
	PARAM_PROLOGUE;
	PARAM_INT(texid);
	ACTION_RETURN_INT(CheckRealHeight(texid));
}

FTextureID FTextureID::operator +(int offset) throw()
{
	if (!isValid()) return *this;
	if (texnum + offset >= TexMan.NumTextures()) return FTextureID(-1);
	return FTextureID(texnum + offset);
}

//texturemanager_short.cpp - part 2 end