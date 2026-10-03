# Big-endian resource inventory

Every disc format the game reads, where the decomp reads it, and how the port converts it.
Paths are under `decomp/` unless they start with `platform/`.
The decomp headers are the layout spec; the converters in this directory follow them field by field.

The port has three ways of handling a big-endian format.

- **Converted in place** (this directory): the loader casts the buffer to structs, so every multi-byte field is byte-swapped once, right after the file is materialised in memory.
- **Kept big-endian**: the loader reads through `JSUInputStream` typed reads, which convert since `decomp-patches/0013`. The few raw `read(void*, n)` / `peek` / `*(u32*)` sites in those loaders get a `decomp-patches/endian-*.patch`.
- **Consumed big-endian by GX**: texture texels, TLUT entries and display lists stay in disc order, because `platform/gx` decodes them big-endian.

Vertex arrays are the exception to the GX rule.
The CPU reads them too (`J3DCluster`, `MapMirror`, `DrawUtil`, `PollutionCount`), so the J3D converter swaps them to host order.
**Do not register converted models with `GXPC_AddBigEndianRange`**; unregistered arrays are already read in host order.

## Table

| Format | Loader (decomp) | Structs (layout spec) | How it is read | Pointer fixups written into the buffer | Port handling |
|---|---|---|---|---|---|
| RARC archive | `JKRMemArchive::open`/`fetchResource` (`libs/JSystem/src/JKernel/JKRMemArchive.cpp`) | `SArcHeader`, `SArcDataInfo`, `SDIDirEntry`, `SDIFileEntry` (`libs/JSystem/include/JSystem/JKernel/JKRArchive.hpp`) | in place | `SDIFileEntry::mData` set at fetch | `platform/misc/endian.cpp` (lead), patch 0011 |
| Yaz0 | `JKRDecomp`, `JKRDvdRipper`, `JKRAram`, `JKRDvdAramRipper` | `SYaz0Header` | length field only | – | patch 0012 (lead) |
| J3D model BMD/BDL, material table BMT (`J3D2 bmd2/bmd3/bmt2/bmt3`) | `J3DModelLoaderDataBase::load`, `J3DModelLoader::load`/`loadMaterialTable` (`libs/JSystem/src/J3D/J3DGraphLoader/J3DModelLoader.cpp`), `J3DJointFactory`, `J3DShapeFactory`, `J3DMaterialFactory(_v21)` | `JUTDataFileHeader`/`JUTDataBlockHeader` (`libs/JSystem/include/JSystem/JUtility/JUTDataHeader.hpp`); `J3DModelInfoBlock`, `J3DVertexBlock`, `J3DEnvelopBlock`, `J3DDrawBlock`, `J3DTextureBlock` (`J3DModelLoader.hpp`); `J3DJointBlock`/`J3DJointInitData` (`J3DJointFactory.hpp`); `J3DShapeBlock`/`J3DShapeInitData`/`J3DShapeMtxInitData`/`J3DShapeDrawInitData` (`J3DShapeFactory.hpp`); `J3DMaterialBlock(_v21)`/`J3DMaterialInitData(_v21)`/`J3DIndInitData` (`J3DMaterialFactory*.hpp`); `J3D*Info` (`J3DGraphBase/J3DStruct.hpp`); `ResTIMG`; `ResNTAB` | in place (`JSUConvertOffsetToPtr` resolves offsets on the fly) | none (the loader keeps pointers in its own objects) | **converted** (`endian_j3d.cpp`): INF1 hierarchy, VTX1 format list + arrays (by component width), EVP1, DRW1, JNT1, SHP1 (not display lists), MAT3/MAT2 (all 30/27 sections), TEX1 ResTIMG headers, name tables. MDL3 is skipped (not read by this J3D). |
| J3D animations (`J3D1` bck1, bca1, btk1, brk1, bpk1, bpa1, btp1, bva1, blk1, bla1, bxk1, bxa1) | `J3DAnmLoaderDataBase::load`, `J3DAnmKeyLoader_v15`, `J3DAnmFullLoader_v15` (`J3DAnmLoader.cpp`) | `J3DAnm*Data` (`J3DAnmLoader.hpp`), `J3DAnm*Table`, `J3DAnmVtxColorIndexData` (`J3DGraphAnimator/J3DAnimation.hpp`) | in place | VCF1/VCK1: `J3DAnmVtxColorIndexData::mpData` rewritten to a pointer (32-bit host OK) | **converted** (`endian_j3d.cpp`): ANK1, ANF1, PAK1, PAF1, TPT1, CLK1, CLF1, VAF1, TTK1 (with post-matrix tables when present), TRK1, VCK1, VCF1. Value arrays by type (f32 / s16 / u8). |
| J3D cluster deform (`bls1`) | `J3DClusterLoader` | `J3DClusterBlock` | in place | cluster index pointers | not converted: the whole TU is dead-stripped in retail (never called) |
| BTI texture (no magic) | `JUTTexture::storeTIMG`, `J2DPicture`, game code via `JKRGetResource` | `ResTIMG` (`libs/JSystem/include/JSystem/ResTIMG.hpp`) | in place | none | **converted by name** (`.bti`), header only; image/palette stay big-endian for GX. Pad byte 0x19 (always 0 on disc, never read) marks a converted header. |
| TLUT (`J2D 'TLUT'`) | `JUTPalette::storeTLUT` | `ResTLUT` | in place | none | **converted by name** (`.bpl`/`.tlut`, header only; byte 4 marks converted). No such file exists on the GMSE01 disc. |
| BFN font (`FONTbfn1`) | `JUTResFont::setResFont` (`libs/JSystem/src/JUtility/JUTResFont.cpp`) | `ResFONT` + INF1/WID1/GLY1/MAP1 (`libs/JSystem/include/JSystem/JUtility/JUTFont.hpp`) | in place | none | `platform/misc/endian.cpp` `bfn_to_native` (lead); matches the code |
| J2D screen BLO (`SCRNblo1`) | `J2DScreen::makeHiearachyPanes`, `J2DPane`/`J2DPicture`/`J2DTextBox`/`J2DWindow` stream constructors | stream layout (no structs) | `JSUMemoryInputStream` typed reads, plus raw `peek`/`read` of magic, block size, pane kind | none | **kept big-endian**; raw sites converted by `endian-02`; colours by `endian-04` |
| BMG text (`MESGbmg1`) | `TMessageLoader` (`src/GC2D/MessageLoader.cpp`), `SMSGetMessageData` (`src/GC2D/MessageUtil.cpp`), `TTalk2D2::setTagParam` | `JMSMesgEntry` (`include/GC2D/MessageLoader.hpp`); header/INF1/DAT1 | mixed: typed stream reads and raw `*(int*)` / raw 12-byte entry copies | none | **kept big-endian**; raw sites converted by `endian-03` |
| JPA particles (`JEFFjpa1`) | `JPAEmitterLoader` (`libs/JSystem/src/JParticle/JPAEmitterLoader.cpp`), `JPABaseShape`, `JPAExtraShape`, `JPASweepShape`, `JPAExTexShape`, `JPADataBlock`, `JPABaseEmitter::loadBaseEmitterBlock`, `JPABaseField::loadFieldBlock` | `JPABinaryHeader`, block headers, `JPATextureData` + `ResTIMG` | BSP1/ESP1/SSP1/ETX1/KFA1/TEX1 in place; BEM1/FLD1 via stream typed reads (after a raw copy) | none | **converted** (`endian_jpa.cpp`) except BEM1/FLD1 bodies, which stay big-endian; BEM1's raw Vec reads converted by `endian-01` |
| AAF JAudio init data (`mSound.aaf`, no magic) | `JAIBasic::checkInitDataOnMemory` (`libs/JSystem/src/JAudio/JAInterface/JAIBasic.cpp`), fed from `TApplication::setupThreadFuncLogo` | command stream of u32 words; payloads below | in place (command walk), payloads copied or parsed | copies only (bank list, scene list) | **converted by name** (`.aaf`, `endian_jaudio.cpp`); self-idempotent (first command word) |
| BST sound tables (AAF cmd 1) | `JAIData::initInfoDataWork`/`setInfoDataPointer` (`JAIData.cpp`) | `JAISoundInfo`, `FabricatedSoundTableMemoryHeader` (`JAIData.hpp`) | in place (copy) | none | converted with the AAF |
| IBNK instrument banks (AAF cmd 2) | `BankMgr::registBankBNK`, `JASBNKParser` | `TInst`, `TKeymap`, `TVmap`, `TOsc`, `TRand`, `TSense`, `TPerc`, `TPmap` (`JASBNKParser.hpp`) | parsed into objects (osc tables copied raw) | none | converted with the AAF (shared structs converted once) |
| WSYS wave systems (AAF cmd 3) | `WaveBankMgr::registWaveBankWS`, `JASWSParser` | `TWaveArchiveBank`, `TWaveArchive`, `TWave`, `TCtrlGroup`, `TCtrlScene`, `TCtrl`, `TCtrlWave` (`JASWSParser.hpp`) | parsed into objects | none | converted with the AAF |
| Sequence archive header (AAF cmd 4) | `JASystem::Vload` (`JASVload.cpp`) | 0x20 header + 0x20 entries | in place (copy) | `+8` written | converted with the AAF |
| Stream list (AAF cmd 5) and .afc stream headers | `JAIGFrameStream` (`JAIGFrameStream.cpp`) | `StreamHeader` | copied from AAF or disc into a struct | none | **kept big-endian**; converted after the copy by `endian-07` |
| Sound/FX scene tables (AAF cmd 6/7) | `JAIBasic`, `JAIData::initFxline` path, `JASDSPInterface` | `FxlineConfig_` (`JASDSPInterface.hpp`) | in place (copy) | scene offsets rebased in the copy | converted with the AAF |
| AAF cmd 8 blob | `JAIBasic::unk78` | unknown | copied | – | left alone: no readers |
| BMS sequences | `JASSeqCtrl`, `JASSeqParser`, `JASTrack` | byte code | byte-wise shift composition (endian-safe) | – | **kept big-endian**; the s16 tables opcodes 0xD7/0xF2 point at are replaced by host-order copies (`endian_seq.cpp`, `endian-10`), 0xED's FIR coefficients converted as copied (`endian-10`) |
| .aw wave archives | `JASWaveArcLoader` → ARAM | ADPCM bytes / PCM16 | DSP | – | nothing for ADPCM; PCM16 samples are big-endian (DSP emulation's concern) |
| BAS animation sound (no magic) | `JAIAnimeSound` (`JAIAnimation.cpp`), `TLiveActor` via `getGlbResource` | `JAIAnimeSoundData`/`JAIAnimeFrameSoundData` (`JAIAnimation.hpp`) | in place | none | **converted by name** (`.bas`); header byte 7 marks converted |
| .prm parameters | `TParams::load` (`src/System/Params.cpp`), `TParamT<T>::load` (`ParamInst.cpp`) | stream layout | typed reads, value read raw | none | **kept big-endian**; value converted by `endian-05` |
| Scene/table .bin (`scene.bin`, `tables.bin`, `PerformLists.bin`, `stageArc.bin`, …) | `JDrama` `TNameRef` loaders via `JSUMemoryInputStream` | stream layout | typed reads | none | **kept big-endian** (0013); string length prefixes by `endian-13`; the raw 4-byte `stream.read(&x, 4)` sites by `endian-08`. Names in these files are Shift-JIS, so game code must be compiled with `-fexec-charset=CP932` for name searches to match. |
| .col map collision (no magic) | `TMapCollisionBase::init` (`src/Map/MapCollisionEntry.cpp`), `MapMakeData.cpp` | header, vertices, 0x18 groups | in place | group offsets → pointers (guarded by the 0x8000 flag) | **converted by name** (`endian_game.cpp`) |
| .ral rails (no magic) | `TGraphGroup` (`src/Enemy/graph.cpp`) | `TRailNode` (`include/Enemy/Graph.hpp`) | in place | none (nodes written back by `translateNodes`) | **converted by name** |
| .ymp pollution map (no magic) | `TPollutionManager` (`src/Map/PollutionManager.cpp`) | `TPollutionInfo`, `TPollutionLayerInfo` | in place | layer and height-map offsets → pointers | **converted by name** |
| .bmp pollution/plane images | `PollutionLayer.cpp`, `MapObjPlane.cpp` | byte access | in place | none | nothing needed |
| .pad Shadow Mario replays (no magic) | `TMarioRecord` (`src/Player/MarioRecord.cpp`) | header + 10 offsets | in place | none | **converted by name** (arrays sized by the gap to the next offset) |
| .bcr movie rumble (no magic) | `MovieRumble.cpp` → `ToolData.cpp` | JMap-style table (`include/MarioUtil/ToolData.hpp`) | in place | none | **converted by name** |
| .sb SPC scripts (`SPCB`) | `TSpcBinary` (`include/Strategic/spcinterp.hpp`), `EventWatcher.cpp`, `livemanager.cpp` | `TSpcHeader`, symbol table | in place | `nameHash`/`nativeCall` written at init | **converted** (header, data table, symbols); byte code kept big-endian, immediates via `endian-06` |
| THP movies | `THPPlayer.c`, `THPRead.c`, `THPVideoDecode.c`, `THPAudioDecode.c`; SDK decoders `THPVideoDecode`/`THPAudioDecode` (`libs/dolphin/src/thp`, stubbed in the port) | `THPHeader`, `THPFrameCompInfo`, `THPVideoInfo`, `THPAudioInfo`, frame header | streamed from disc into buffers | none | header/infos/frame-offset words/frame headers converted by `endian-12`. Component payloads stay big-endian: a PC `THPVideoDecode` must load the entropy-coded bitstream as big-endian u32 words, and a PC `THPAudioDecode` must read `THPAudioRecordHeader` (u32 ×2, s16 coefs/history) big-endian. Audio data per frame is component size × `sndNumTracks`. |
| Memory card save | `TFlagManager::save`/`load`/`saveOption`/`loadOption`, `TCardSector` (`CardManager.cpp`), bookmark preview via typed reads | `TCardSector`, `TCardBookmarkInfo` | stream | – | **retail big-endian on card** (`endian-11`): both sides agree; sectors stay in card byte order in memory |
| `mSound.asn`, `.me`, `/mario.MAP` | – | – | not read / text | – | nothing needed |

## Blockers found on the boot path that are not byte order

- **Shift-JIS literals.** JDrama looks objects up by name, and the names in `stageArc.bin`/scene `.bin` are Shift-JIS (`ステージ毎シナリオアーカイブ名群` is at offset 77 of `stageArc.bin` in CP932, absent in UTF-8). GCC encodes the decomp's literals as UTF-8, so every Japanese-name search fails (first crash: `TApplication::mountStageArchive`). Building the game with `-fexec-charset=CP932` (C and C++) fixes it; verified in a private build that then boots into the file-select screen load.
- **Decomp bug in `TCardLoad::load` (`src/GC2D/CardLoad.cpp:361`)**: `search('sh0k' + i)` must be `search('sh0k' + i * 0x100)`. `load_score.blo` has `sh0k`..`sh6k`, and retail (`8016F760`, `addis r4, r22, 0x7368` with `r22 += 0x100` per file) steps by 0x100. The PC build dereferences the null pane for i = 1. This needs fixing in the decomp.

## Other big-endian assumptions found on the way

- `JUtility::TColor::set(u32)`/`toUInt32()` stored the u32 by memory layout; `endian-04` makes them value-based (`0xRRGGBBAA`), which fixes colours from typed reads and colour literals.
- `MarioUtil/DrawUtil.cpp:744` reads `*(u16*)(p + 1)` from a J3D display list (vertex count); `endian-09` makes it a big-endian load.
- `J3DSkinDeform::initMtxIndexArray` (`J3DCluster.cpp`) also walks display lists (vertex count, u16 position/normal indices); `endian-14` reads them big-endian.
- JDrama names in `.bin` files are Shift-JIS; the MWCC build encodes literals as Shift-JIS (sjiswrap), so the GCC build needs `-fexec-charset=CP932` (CMake, not a patch).
- `JUTNameTab` hashes names with `char`, which is signed on x86 as on MWCC; only non-ASCII names could differ.
