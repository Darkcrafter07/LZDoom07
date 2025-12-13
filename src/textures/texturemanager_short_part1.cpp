//texturemanager_short.cpp - part 1 start

FTextureManager TexMan;

CUSTOM_CVAR(Bool, vid_nopalsubstitutions, false, CVAR_ARCHIVE)
{
	// This is in case the sky texture has been substituted.
	R_InitSkyMap ();
}

FTextureManager::FTextureManager ()
{
	memset (HashFirst, -1, sizeof(HashFirst));

	for (int i = 0; i < 2048; ++i)
	{
		sintable[i] = short(sin(i*(M_PI / 1024)) * 16384);
	}
}

FTextureManager::~FTextureManager ()
{
	DeleteAll();
}

void FTextureManager::DeleteAll()
{
	for (unsigned int i = 0; i < Textures.Size(); ++i)
	{
		delete Textures[i].Texture;
	}
	Textures.Clear();
	Translation.Clear();
	FirstTextureForFile.Clear();
	memset (HashFirst, -1, sizeof(HashFirst));
	DefaultTexture.SetInvalid();

	for (unsigned i = 0; i < mAnimations.Size(); i++)
	{
		if (mAnimations[i] != NULL)
		{
			M_Free (mAnimations[i]);
			mAnimations[i] = NULL;
		}
	}
	mAnimations.Clear();

	for (unsigned i = 0; i < mSwitchDefs.Size(); i++)
	{
		if (mSwitchDefs[i] != NULL)
		{
			M_Free (mSwitchDefs[i]);
			mSwitchDefs[i] = NULL;
		}
	}
	mSwitchDefs.Clear();

	for (unsigned i = 0; i < mAnimatedDoors.Size(); i++)
	{
		if (mAnimatedDoors[i].TextureFrames != NULL)
		{
			delete[] mAnimatedDoors[i].TextureFrames;
			mAnimatedDoors[i].TextureFrames = NULL;
		}
	}
	mAnimatedDoors.Clear();
	BuildTileData.Clear();
}

FTextureID FTextureManager::CheckForTexture (const char *name, ETextureType usetype, BITFIELD flags)
{
	int i;
	int firstfound = -1;
	auto firsttype = ETextureType::Null;

	if (name == NULL || name[0] == '\0')
	{
		return FTextureID(-1);
	}
	// [RH] Doom counted anything beginning with '-' as "no texture".
	// Hopefully nobody made use of that and had textures like "-EMPTY",
	// because -NOFLAT- is a valid graphic for ZDoom.
	if (name[0] == '-' && name[1] == '\0')
	{
		return FTextureID(0);
	}
	i = HashFirst[MakeKey (name) % HASH_SIZE];

	while (i != HASH_END)
	{
		const FTexture *tex = Textures[i].Texture;

		if (stricmp (tex->Name, name) == 0)
		{
			// The name matches, so check the texture type
			if (usetype == ETextureType::Any)
			{
				// All NULL textures should actually return 0
				if (tex->UseType == ETextureType::FirstDefined && !(flags & TEXMAN_ReturnFirst)) return 0;
				if (tex->UseType == ETextureType::SkinGraphic && !(flags & TEXMAN_AllowSkins)) return 0;
				return FTextureID(tex->UseType==ETextureType::Null ? 0 : i);
			}
			else if ((flags & TEXMAN_Overridable) && tex->UseType == ETextureType::Override)
			{
				return FTextureID(i);
			}
			else if (tex->UseType == usetype)
			{
				return FTextureID(i);
			}
			else if (tex->UseType == ETextureType::FirstDefined && usetype == ETextureType::Wall)
			{
				if (!(flags & TEXMAN_ReturnFirst)) return FTextureID(0);
				else return FTextureID(i);
			}
			else if (tex->UseType == ETextureType::Null && usetype == ETextureType::Wall)
			{
				// We found a NULL texture on a wall -> return 0
				return FTextureID(0);
			}
			else
			{
				if (firsttype == ETextureType::Null ||
					(firsttype == ETextureType::MiscPatch &&
					 tex->UseType != firsttype &&
					 tex->UseType != ETextureType::Null)
				   )
				{
					firstfound = i;
					firsttype = tex->UseType;
				}
			}
		}
		i = Textures[i].HashNext;
	}

	if ((flags & TEXMAN_TryAny) && usetype != ETextureType::Any)
	{
		// Never return the index of NULL textures.
		if (firstfound != -1)
		{
			if (firsttype == ETextureType::Null) return FTextureID(0);
			if (firsttype == ETextureType::FirstDefined && !(flags & TEXMAN_ReturnFirst)) return FTextureID(0);
			return FTextureID(firstfound);
		}
	}

	
	if (!(flags & TEXMAN_ShortNameOnly))
	{
		// We intentionally only look for textures in subdirectories.
		// Any graphic being placed in the zip's root directory can not be found by this.
		if (strchr(name, '/'))
		{
			FTexture *const NO_TEXTURE = (FTexture*)-1;
			int lump = Wads.CheckNumForFullName(name);
			if (lump >= 0)
			{
				FTexture *tex = Wads.GetLinkedTexture(lump);
				if (tex == NO_TEXTURE) return FTextureID(-1);
				if (tex != NULL) return tex->id;
				if (flags & TEXMAN_DontCreate) return FTextureID(-1);	// we only want to check, there's no need to create a texture if we don't have one yet.
				tex = FTexture::CreateTexture("", lump, ETextureType::Override);
				if (tex != NULL)
				{
					Wads.SetLinkedTexture(lump, tex);
					return AddTexture(tex);
				}
				else
				{
					// mark this lump as having no valid texture so that we don't have to retry creating one later.
					Wads.SetLinkedTexture(lump, NO_TEXTURE);
				}
			}
		}
	}

	return FTextureID(-1);
}

static int CheckForTexture(const FString &name, int type, int flags)
{
	return TexMan.CheckForTexture(name, static_cast<ETextureType>(type), flags).GetIndex();
}

DEFINE_ACTION_FUNCTION_NATIVE(_TexMan, CheckForTexture, CheckForTexture)
{
	PARAM_PROLOGUE;
	PARAM_STRING(name);
	PARAM_INT(type);
	PARAM_INT(flags);
	ACTION_RETURN_INT(CheckForTexture(name, type, flags));
}

int FTextureManager::ListTextures (const char *name, TArray<FTextureID> &list, bool listall)
{
	int i;

	if (name == NULL || name[0] == '\0')
	{
		return 0;
	}
	// [RH] Doom counted anything beginning with '-' as "no texture".
	// Hopefully nobody made use of that and had textures like "-EMPTY",
	// because -NOFLAT- is a valid graphic for ZDoom.
	if (name[0] == '-' && name[1] == '\0')
	{
		return 0;
	}
	i = HashFirst[MakeKey (name) % HASH_SIZE];

	while (i != HASH_END)
	{
		const FTexture *tex = Textures[i].Texture;

		if (stricmp (tex->Name, name) == 0)
		{
			// NULL textures must be ignored.
			if (tex->UseType!=ETextureType::Null) 
			{
				unsigned int j = list.Size();
				if (!listall)
				{
					for (j = 0; j < list.Size(); j++)
					{
						// Check for overriding definitions from newer WADs
						if (Textures[list[j].GetIndex()].Texture->UseType == tex->UseType) break;
					}
				}
				if (j==list.Size()) list.Push(FTextureID(i));
			}
		}
		i = Textures[i].HashNext;
	}
	return list.Size();
}

FTextureID FTextureManager::GetTexture (const char *name, ETextureType usetype, BITFIELD flags)
{
	FTextureID i;

	if (name == NULL || name[0] == 0)
	{
		return FTextureID(0);
	}
	else
	{
		i = CheckForTexture (name, usetype, flags | TEXMAN_TryAny);
	}

	if (!i.Exists())
	{
		// Use a default texture instead of aborting like Doom did
		Printf ("Unknown texture: \"%s\"\n", name);
		i = DefaultTexture;
	}
	return FTextureID(i);
}

FTexture *FTextureManager::FindTexture(const char *texname, ETextureType usetype, BITFIELD flags)
{
	FTextureID texnum = CheckForTexture (texname, usetype, flags);
	return !texnum.isValid()? NULL : Textures[texnum.GetIndex()].Texture;
}

void FTextureManager::UnloadAll ()
{
	for (unsigned int i = 0; i < Textures.Size(); ++i)
	{
		Textures[i].Texture->Unload ();
	}
}

FTextureID FTextureManager::AddTexture (FTexture *texture)
{
	int bucket;
	int hash;

	if (texture == NULL) return FTextureID(-1);

	// Later textures take precedence over earlier ones

	// Textures without name can't be looked for
	if (texture->Name[0] != '\0')
	{
		bucket = int(MakeKey (texture->Name) % HASH_SIZE);
		hash = HashFirst[bucket];
	}
	else
	{
		bucket = -1;
		hash = -1;
	}

	TextureHash hasher = { texture, hash };
	int trans = Textures.Push (hasher);
	Translation.Push (trans);
	if (bucket >= 0) HashFirst[bucket] = trans;
	return (texture->id = FTextureID(trans));
}

FTextureID FTextureManager::CreateTexture (int lumpnum, ETextureType usetype)
{
	if (lumpnum != -1)
	{
		FTexture *out = FTexture::CreateTexture(lumpnum, usetype);

		if (out != NULL) return AddTexture (out);
		else
		{
			Printf (TEXTCOLOR_ORANGE "Invalid data encountered for texture %s\n", Wads.GetLumpFullPath(lumpnum).GetChars());
			return FTextureID(-1);
		}
	}
	return FTextureID(-1);
}

void FTextureManager::ReplaceTexture (FTextureID picnum, FTexture *newtexture, bool free)
{
	int index = picnum.GetIndex();
	if (unsigned(index) >= Textures.Size())
		return;

	FTexture *oldtexture = Textures[index].Texture;

	newtexture->Name = oldtexture->Name;
	newtexture->UseType = oldtexture->UseType;
	Textures[index].Texture = newtexture;

	newtexture->id = oldtexture->id;
	if (free && !oldtexture->bKeepAround)
	{
		delete oldtexture;
	}
	else
	{
		oldtexture->id.SetInvalid();
	}
}

bool FTextureManager::AreTexturesCompatible (FTextureID picnum1, FTextureID picnum2)
{
	int index1 = picnum1.GetIndex();
	int index2 = picnum2.GetIndex();
	if (unsigned(index1) >= Textures.Size() || unsigned(index2) >= Textures.Size())
		return false;

	FTexture *texture1 = Textures[index1].Texture;
	FTexture *texture2 = Textures[index2].Texture;

	// both textures must be the same type.
	if (texture1 == NULL || texture2 == NULL || texture1->UseType != texture2->UseType)
		return false;

	// both textures must be from the same file
	for(unsigned i = 0; i < FirstTextureForFile.Size() - 1; i++)
	{
		if (index1 >= FirstTextureForFile[i] && index1 < FirstTextureForFile[i+1])
		{
			return (index2 >= FirstTextureForFile[i] && index2 < FirstTextureForFile[i+1]);
		}
	}
	return false;
}

void FTextureManager::AddGroup(int wadnum, int ns, ETextureType usetype)
{
	int firsttx = Wads.GetFirstLump(wadnum);
	int lasttx = Wads.GetLastLump(wadnum);
	FString Name;

	// Go from first to last so that ANIMDEFS work as expected. However,
	// to avoid duplicates (and to keep earlier entries from overriding
	// later ones), the texture is only inserted if it is the one returned
	// by doing a check by name in the list of wads.

	for (; firsttx <= lasttx; ++firsttx)
	{
		if (Wads.GetLumpNamespace(firsttx) == ns)
		{
			Wads.GetLumpName (Name, firsttx);

			if (Wads.CheckNumForName (Name, ns) == firsttx)
			{
				CreateTexture (firsttx, usetype);
			}
			StartScreen->Progress();
		}
		else if (ns == ns_flats && Wads.GetLumpFlags(firsttx) & LUMPF_MAYBEFLAT)
		{
			if (Wads.CheckNumForName (Name, ns) < firsttx)
			{
				CreateTexture (firsttx, usetype);
			}
			StartScreen->Progress();
		}
	}
}

// Adds all hires texture definitions.
void FTextureManager::AddHiresTextures (int wadnum)
{
	int firsttx = Wads.GetFirstLump(wadnum);
	int lasttx = Wads.GetLastLump(wadnum);

	FString Name;
	TArray<FTextureID> tlist;

	if (firsttx == -1 || lasttx == -1)
	{
		return;
	}

	for (;firsttx <= lasttx; ++firsttx)
	{
		if (Wads.GetLumpNamespace(firsttx) == ns_hires)
		{
			Wads.GetLumpName (Name, firsttx);

			if (Wads.CheckNumForName (Name, ns_hires) == firsttx)
			{
				tlist.Clear();
				int amount = ListTextures(Name, tlist);
				if (amount == 0)
				{
					// A texture with this name does not yet exist
					FTexture * newtex = FTexture::CreateTexture (firsttx, ETextureType::Any);
					if (newtex != NULL)
					{
						newtex->UseType=ETextureType::Override;
						AddTexture(newtex);
					}
				}
				else
				{
					for(unsigned int i = 0; i < tlist.Size(); i++)
					{
						FTexture * newtex = FTexture::CreateTexture (firsttx, ETextureType::Any);
						if (newtex != NULL)
						{
							FTexture * oldtex = Textures[tlist[i].GetIndex()].Texture;

							// Replace the entire texture and adjust the scaling and offset factors.
							newtex->bWorldPanning = true;
							newtex->SetScaledSize(oldtex->GetScaledWidth(), oldtex->GetScaledHeight());
							newtex->LeftOffset = int(oldtex->GetScaledLeftOffset() * newtex->Scale.X);
							newtex->TopOffset = int(oldtex->GetScaledTopOffset() * newtex->Scale.Y);
							ReplaceTexture(tlist[i], newtex, true);
						}
					}
				}
				StartScreen->Progress();
			}
		}
	}
}

void FTextureManager::LoadTextureDefs(int wadnum, const char *lumpname)
{
	int remapLump, lastLump;

	lastLump = 0;

	while ((remapLump = Wads.FindLump(lumpname, &lastLump)) != -1)
	{
		if (Wads.GetLumpFile(remapLump) == wadnum)
		{
			ParseTextureDef(remapLump);
		}
	}
}

void FTextureManager::ParseTextureDef(int lump)
{
	TArray<FTextureID> tlist;

	FScanner sc(lump);
	while (sc.GetString())
	{
		if (sc.Compare("remap")) // remap an existing texture
		{
			sc.MustGetString();

			// allow selection by type
			int mode;
			ETextureType type;
			if (sc.Compare("wall")) type=ETextureType::Wall, mode=FTextureManager::TEXMAN_Overridable;
			else if (sc.Compare("flat")) type=ETextureType::Flat, mode=FTextureManager::TEXMAN_Overridable;
			else if (sc.Compare("sprite")) type=ETextureType::Sprite, mode=0;
			else type = ETextureType::Any, mode = 0;

			if (type != ETextureType::Any) sc.MustGetString();

			sc.String[8]=0;

			tlist.Clear();
			int amount = ListTextures(sc.String, tlist);
			FName texname = sc.String;

			sc.MustGetString();
			int lumpnum = Wads.CheckNumForFullName(sc.String, true, ns_patches);
			if (lumpnum == -1) lumpnum = Wads.CheckNumForFullName(sc.String, true, ns_graphics);

			if (tlist.Size() == 0)
			{
				Printf("Attempting to remap non-existent texture %s to %s\n",
					texname.GetChars(), sc.String);
			}
			else if (lumpnum == -1)
			{
				Printf("Attempting to remap texture %s to non-existent lump %s\n",
					texname.GetChars(), sc.String);
			}
			else
			{
				for(unsigned int i = 0; i < tlist.Size(); i++)
				{
					FTexture * oldtex = Textures[tlist[i].GetIndex()].Texture;
					int sl;

					// only replace matching types. For sprites also replace any MiscPatches
					// based on the same lump. These can be created for icons.
					if (oldtex->UseType == type || type == ETextureType::Any ||
						(mode == TEXMAN_Overridable && oldtex->UseType == ETextureType::Override) ||
						(type == ETextureType::Sprite && oldtex->UseType == ETextureType::MiscPatch &&
						(sl=oldtex->GetSourceLump()) >= 0 && Wads.GetLumpNamespace(sl) == ns_sprites)
						)
					{
						FTexture * newtex = FTexture::CreateTexture (lumpnum, ETextureType::Any);
						if (newtex != NULL)
						{
							// Replace the entire texture and adjust the scaling and offset factors.
							newtex->bWorldPanning = true;
							newtex->SetScaledSize(oldtex->GetScaledWidth(), oldtex->GetScaledHeight());
							newtex->LeftOffset = int(oldtex->GetScaledLeftOffset() * newtex->Scale.X);
							newtex->TopOffset = int(oldtex->GetScaledTopOffset() * newtex->Scale.Y);
							ReplaceTexture(tlist[i], newtex, true);
						}
					}
				}
			}
		}
		else if (sc.Compare("define")) // define a new "fake" texture
		{
			sc.GetString();
					
			FString base = ExtractFileBase(sc.String, false);
			if (!base.IsEmpty())
			{
				FString src = base.Left(8);

				int lumpnum = Wads.CheckNumForFullName(sc.String, true, ns_patches);
				if (lumpnum == -1) lumpnum = Wads.CheckNumForFullName(sc.String, true, ns_graphics);

				sc.GetString();
				bool is32bit = !!sc.Compare("force32bit");
				if (!is32bit) sc.UnGet();

				sc.MustGetNumber();
				int width = sc.Number;
				sc.MustGetNumber();
				int height = sc.Number;

				if (lumpnum>=0)
				{
					FTexture *newtex = FTexture::CreateTexture(lumpnum, ETextureType::Override);

					if (newtex != NULL)
					{
						// Replace the entire texture and adjust the scaling and offset factors.
						newtex->bWorldPanning = true;
						newtex->SetScaledSize(width, height);
						newtex->Name = src;

						FTextureID oldtex = TexMan.CheckForTexture(src, ETextureType::MiscPatch);
						if (oldtex.isValid()) 
						{
							ReplaceTexture(oldtex, newtex, true);
							newtex->UseType = ETextureType::Override;
						}
						else AddTexture(newtex);
					}
				}
			}				
			//else Printf("Unable to define hires texture '%s'\n", tex->Name);
		}
		else if (sc.Compare("texture"))
		{
			ParseXTexture(sc, ETextureType::Override);
		}
		else if (sc.Compare("sprite"))
		{
			ParseXTexture(sc, ETextureType::Sprite);
		}
		else if (sc.Compare("walltexture"))
		{
			ParseXTexture(sc, ETextureType::Wall);
		}
		else if (sc.Compare("flat"))
		{
			ParseXTexture(sc, ETextureType::Flat);
		}
		else if (sc.Compare("graphic"))
		{
			ParseXTexture(sc, ETextureType::MiscPatch);
		}
		else if (sc.Compare("#include"))
		{
			sc.MustGetString();

			// This is not using sc.Open because it can print a more useful error message when done here
			int includelump = Wads.CheckNumForFullName(sc.String, true);
			if (includelump == -1)
			{
				sc.ScriptError("Lump '%s' not found", sc.String);
			}
			else
			{
				ParseTextureDef(includelump);
			}
		}
		else
		{
			sc.ScriptError("Texture definition expected, found '%s'", sc.String);
		}
	}
}

//texturemanager_short.cpp - part 1 end