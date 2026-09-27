#include <JSystem/J2D/J2DOrthoGraph.hxx>
#include <JSystem/J2D/J2DPane.hxx>
#include <JSystem/J2D/J2DPicture.hxx>
#include <JSystem/J2D/J2DPrint.hxx>
#include <JSystem/J2D/J2DScreen.hxx>
#include <JSystem/J2D/J2DSetScreen.hxx>
#include <JSystem/J2D/J2DTextBox.hxx>
#include <JSystem/J2D/J2DWindow.hxx>
#include <JSystem/J3D/J3DAnimation.hxx>
#include <JSystem/J3D/J3DAnmLoader.hxx>
#include <JSystem/J3D/J3DCluster.hxx>
#include <JSystem/J3D/J3DColor.hxx>
#include <JSystem/J3D/J3DDrawBuffer.hxx>
#include <JSystem/J3D/J3DJoint.hxx>
#include <JSystem/J3D/J3DMaterial.hxx>
#include <JSystem/J3D/J3DModel.hxx>
#include <JSystem/J3D/J3DModelLoader.hxx>
#include <JSystem/J3D/J3DModelLoaderDataBase.hxx>
#include <JSystem/J3D/J3DNode.hxx>
#include <JSystem/J3D/J3DPacket.hxx>
#include <JSystem/J3D/J3DShape.hxx>
#include <JSystem/J3D/J3DTexture.hxx>
#include <JSystem/J3D/J3DTransform.hxx>
#include <JSystem/J3D/J3DVertex.hxx>
#include <SMS/Camera/CamConnector.hxx>
#include <SMS/Camera/CamSaveKindParam.hxx>
#include <SMS/Camera/CameraInbetween.hxx>
#include <SMS/Camera/CameraKindParam.hxx>
#include <SMS/Camera/CameraMapTool.hxx>
#include <SMS/Camera/CameraMarioData.hxx>
#include <SMS/Camera/CameraShake.hxx>
#include <SMS/Camera/CubeManagerArea.hxx>
#include <SMS/Camera/CubeManagerFast.hxx>
#include <SMS/Camera/CubeManagerMarioIn.hxx>
#include <SMS/Camera/CubeMapTool.hxx>
#include <SMS/Camera/LensFlare.hxx>
#include <SMS/Camera/LensGlow.hxx>
#include <SMS/Camera/PolarSubCamera.hxx>
#include <SMS/Camera/SunModel.hxx>
#include <SMS/Enemy/BossGesso.hxx>
#include <SMS/Enemy/BossManta.hxx>
#include <SMS/Enemy/Conductor.hxx>
#include <SMS/Enemy/EffectObjBase.hxx>
#include <SMS/Enemy/EnemyMario.hxx>
#include <SMS/Enemy/HamuKuri.hxx>
#include <SMS/Enemy/Kukku.hxx>
#include <SMS/Enemy/SmallEnemy.hxx>
#include <SMS/Enemy/SpineBase.hxx>
#include <SMS/Enemy/SpineEnemy.hxx>
#include <SMS/Enemy/WalkerEnemy.hxx>
#include <SMS/G2D/BoundPane.hxx>
#include <SMS/G2D/ExPane.hxx>
#include <SMS/GC2D/CardLoad.hxx>
#include <SMS/GC2D/ConsoleStr.hxx>
#include <SMS/GC2D/Coord2D.hxx>
#include <SMS/GC2D/GCConsole2.hxx>
#include <SMS/GC2D/Guide.hxx>
#include <SMS/GC2D/OptionControl.hxx>
#include <SMS/GC2D/OptionRumbleUnit.hxx>
#include <SMS/GC2D/OptionSoundUnit.hxx>
#include <SMS/GC2D/OptionSubtitleUnit.hxx>
#include <SMS/GC2D/SMSFader.hxx>
#include <SMS/GC2D/SelectDir.hxx>
#include <SMS/GC2D/SelectGrad.hxx>
#include <SMS/GC2D/SelectMenu.hxx>
#include <SMS/GC2D/SelectShine.hxx>
#include <SMS/GC2D/ShineFader.hxx>
#include <SMS/GC2D/SmplFader.hxx>
#include <SMS/GC2D/SunGlass.hxx>
#include <SMS/GC2D/Talk2D2.hxx>
#include <SMS/Graph/GraphGroup.hxx>
#include <SMS/Graph/GraphNode.hxx>
#include <SMS/Graph/GraphTracer.hxx>
#include <SMS/Graph/GraphWeb.hxx>
#include <SMS/Graph/RailNode.hxx>
#include <SMS/Graph/SplineRail.hxx>
#include <SMS/M3DUtil/LodAnm.hxx>
#include <SMS/M3DUtil/M3UModel.hxx>
#include <SMS/M3DUtil/MActor.hxx>
#include <SMS/M3DUtil/MActorKeeper.hxx>
#include <SMS/M3DUtil/SDLModel.hxx>
#include <SMS/MSound/MAnmSound.hxx>
#include <SMS/MSound/MSBGM.hxx>
#include <SMS/MSound/MSModBgm.hxx>
#include <SMS/MSound/MSound.hxx>
#include <SMS/MSound/MSoundSESystem.hxx>
#include <SMS/Manager/EffectObjManager.hxx>
#include <SMS/Manager/EnemyManager.hxx>
#include <SMS/Manager/FlagManager.hxx>
#include <SMS/Manager/ItemManager.hxx>
#include <SMS/Manager/JointModelManager.hxx>
#include <SMS/Manager/LiveManager.hxx>
#include <SMS/Manager/MapCollisionManager.hxx>
#include <SMS/Manager/MarioParticleManager.hxx>
#include <SMS/Manager/ModelWaterManager.hxx>
#include <SMS/Manager/ObjManager.hxx>
#include <SMS/Manager/PollutionManager.hxx>
#include <SMS/Manager/RumbleManager.hxx>
#include <SMS/Manager/SelectShineManager.hxx>
#include <SMS/Map/BGCheck.hxx>
#include <SMS/Map/JointModel.hxx>
#include <SMS/Map/JointObj.hxx>
#include <SMS/Map/Map.hxx>
#include <SMS/Map/MapCheck.hxx>
#include <SMS/Map/MapCollisionBase.hxx>
#include <SMS/Map/MapCollisionData.hxx>
#include <SMS/Map/MapCollisionMove.hxx>
#include <SMS/Map/MapCollisionStatic.hxx>
#include <SMS/Map/MapCollisionWarp.hxx>
#include <SMS/Map/MapMakeList.hxx>
#include <SMS/Map/PollutionLayer.hxx>
#include <SMS/Map/PollutionObj.hxx>
#include <SMS/Map/PollutionPos.hxx>
#include <SMS/MapObj/MapObjBall.hxx>
#include <SMS/MapObj/MapObjBase.hxx>
#include <SMS/MapObj/MapObjGeneral.hxx>
#include <SMS/MapObj/MapObjInit.hxx>
#include <SMS/MapObj/MapObjNormalLift.hxx>
#include <SMS/MapObj/MapObjRail.hxx>
#include <SMS/MapObj/MapObjRailBlock.hxx>
#include <SMS/MapObj/MapObjTree.hxx>
#include <SMS/MapObj/MapObjWave.hxx>
#include <SMS/MapObj/MapObjWoodBlock.hxx>
#include <SMS/MarioUtil/DrawUtil.hxx>
#include <SMS/MarioUtil/LightUtil.hxx>
#include <SMS/MarioUtil/MathUtil.hxx>
#include <SMS/MarioUtil/RidingInfo.hxx>
#include <SMS/MarioUtil/ShadowUtil.hxx>
#include <SMS/MarioUtil/TexUtil.hxx>
#include <SMS/MarioUtil/gd-reinit-gx.hxx>
#include <SMS/MoveBG/Coin.hxx>
#include <SMS/MoveBG/DemoCannon.hxx>
#include <SMS/MoveBG/EggYoshi.hxx>
#include <SMS/MoveBG/Item.hxx>
#include <SMS/MoveBG/Mushroom1up.hxx>
#include <SMS/MoveBG/NozzleBox.hxx>
#include <SMS/MoveBG/ResetFruit.hxx>
#include <SMS/MoveBG/Shine.hxx>
#include <SMS/NPC/NpcBase.hxx>
#include <SMS/Player/Mario.hxx>
#include <SMS/Player/MarioBlend.hxx>
#include <SMS/Player/MarioCap.hxx>
#include <SMS/Player/MarioDraw.hxx>
#include <SMS/Player/MarioEffect.hxx>
#include <SMS/Player/MarioGamePad.hxx>
#include <SMS/Player/MarioSound.hxx>
#include <SMS/Player/NozzleBase.hxx>
#include <SMS/Enemy/BossPakkun.hxx>
#include <SMS/Player/NozzleDeform.hxx>
#include <SMS/Player/NozzleTrigger.hxx>
#include <SMS/Player/Watergun.hxx>
#include <SMS/Player/Yoshi.hxx>
#include <SMS/SPC/SpcBinary.hxx>
#include <SMS/SPC/SpcInterp.hxx>
#include <SMS/SPC/SpcSlice.hxx>
#include <SMS/SPC/SpcStack.hxx>
#include <SMS/SPC/SpcTypedBinary.hxx>
#include <SMS/SPC/SpcTypedInterp.hxx>
#include <SMS/Strategic/HitActor.hxx>
#include <SMS/Strategic/LiveActor.hxx>
#include <SMS/Strategic/ObjChara.hxx>
#include <SMS/Strategic/ObjHitCheck.hxx>
#include <SMS/Strategic/Strategy.hxx>
#include <SMS/Strategic/TakeActor.hxx>
#include <SMS/System/Application.hxx>
#include <SMS/System/BaseParam.hxx>
#include <SMS/System/CardManager.hxx>
#include <SMS/System/EventWatcher.hxx>
#include <SMS/System/GCLogoDir.hxx>
#include <SMS/System/GameSequence.hxx>
#include <SMS/System/MarDirector.hxx>
#include <SMS/System/MarNameRefGen.hxx>
#include <SMS/System/MenuDirector.hxx>
#include <SMS/System/MovieDirector.hxx>
#include <SMS/System/Params.hxx>
#include <SMS/System/PerformList.hxx>
#include <SMS/System/RenderModeObj.hxx>
#include <SMS/System/Resolution.hxx>
#include <SMS/System/SnapTimeObj.hxx>
#include <SMS/System/TargetArrow.hxx>
#include <SMS/assert.h>
#include <SMS/macros.h>
#include <SMS/rand.h>
CPolarSubCamera *probe_0 = 0;
J2DGrafContext *probe_1 = 0;
J2DOrthoGraph *probe_2 = 0;
J2DPane *probe_3 = 0;
J2DPicture *probe_4 = 0;
J2DPrint *probe_5 = 0;
J2DScreen *probe_6 = 0;
J2DSetScreen *probe_7 = 0;
J2DTextBox *probe_8 = 0;
J2DWindow *probe_9 = 0;
J3DAnmBase *probe_10 = 0;
J3DAnmCluster *probe_11 = 0;
J3DAnmClusterFull *probe_12 = 0;
J3DAnmClusterKey *probe_13 = 0;
J3DAnmColor *probe_14 = 0;
J3DAnmKeyTableBase *probe_15 = 0;
J3DAnmTevRegKey *probe_16 = 0;
J3DAnmTexPattern *probe_17 = 0;
J3DAnmTextureSRTKey *probe_18 = 0;
J3DAnmTransform *probe_19 = 0;
J3DAnmTransformFull *probe_20 = 0;
J3DAnmTransformKey *probe_21 = 0;
J3DCallbackPacket *probe_22 = 0;
J3DColorBlock *probe_23 = 0;
J3DColorChan *probe_24 = 0;
J3DDisplayListObj *probe_25 = 0;
J3DDrawBuffer *probe_26 = 0;
J3DDrawMtxData *probe_27 = 0;
J3DDrawPacket *probe_28 = 0;
J3DFrameCtrl *probe_29 = 0;
J3DIndBlock *probe_30 = 0;
J3DIndTevStage *probe_31 = 0;
J3DJoint *probe_32 = 0;
J3DMatColorAnm *probe_33 = 0;
J3DMatPacket *probe_34 = 0;
J3DMaterial *probe_35 = 0;
J3DMaterialBlock *probe_36 = 0;
J3DMaterialBlock_v21 *probe_37 = 0;
J3DMaterialTable *probe_38 = 0;
J3DModel *probe_39 = 0;
J3DModelData *probe_40 = 0;
J3DModelHierarchy *probe_41 = 0;
J3DModelLoader *probe_42 = 0;
J3DModelLoaderDataBase *probe_43 = 0;
J3DModelLoader_v21 *probe_44 = 0;
J3DModelLoader_v26 *probe_45 = 0;
J3DNode *probe_46 = 0;
J3DPEBlock *probe_47 = 0;
J3DPacket *probe_48 = 0;
J3DShape *probe_49 = 0;
J3DShapeMtx *probe_50 = 0;
J3DShapePacket *probe_51 = 0;
J3DSkinDeform *probe_52 = 0;
J3DTevBlock *probe_53 = 0;
J3DTevBlock1 *probe_54 = 0;
J3DTevBlock16 *probe_55 = 0;
J3DTevBlock2 *probe_56 = 0;
J3DTevBlock4 *probe_57 = 0;
J3DTevKColorAnm *probe_58 = 0;
J3DTevOrder *probe_59 = 0;
J3DTevStage *probe_60 = 0;
J3DTevSwapModeInfo *probe_61 = 0;
J3DTevSwapModeTable *probe_62 = 0;
J3DTexBlock *probe_63 = 0;
J3DTexCoord *probe_64 = 0;
J3DTexGenBlock *probe_65 = 0;
J3DTexNoAnm *probe_66 = 0;
J3DTexNtxAnm *probe_67 = 0;
J3DTexture *probe_68 = 0;
J3DTransformInfo *probe_69 = 0;
J3DVertexBuffer *probe_70 = 0;
J3DVertexData *probe_71 = 0;
JAIActor *probe_72 = 0;
JAIAnimeSound *probe_73 = 0;
JAIBasic *probe_74 = 0;
JAIMoveParaSet *probe_75 = 0;
JAISound *probe_76 = 0;
JDrama::TActor *probe_80 = 0;
JDrama::TCamera *probe_82 = 0;
JDrama::TCharacter *probe_83 = 0;
JDrama::TDStageDisp *probe_84 = 0;
JDrama::TDirector *probe_86 = 0;
JDrama::TDisplay *probe_87 = 0;
JDrama::TEfbCtrl *probe_89 = 0;
JDrama::TEfbCtrlDisp *probe_90 = 0;
JDrama::TEfbCtrlTex *probe_91 = 0;
JDrama::TGraphics *probe_93 = 0;
JDrama::TLookAtCamera *probe_96 = 0;
JDrama::TNameRef *probe_97 = 0;
JDrama::TNameRefGen *probe_98 = 0;
JDrama::TOrthoProj *probe_99 = 0;
JDrama::TPlacement *probe_100 = 0;
JDrama::TPolarCamera *probe_101 = 0;
JDrama::TRect *probe_102 = 0;
JDrama::TScreen *probe_103 = 0;
JDrama::TVideo *probe_105 = 0;
JDrama::TViewConnecter *probe_106 = 0;
JDrama::TViewObj *probe_107 = 0;
JDrama::TViewport *probe_108 = 0;
JGadget::TLinkListNode *probe_109 = 0;
JGadget::TList_pointer_void *probe_110 = 0;
JGadget::TNodeLinkList *probe_111 = 0;
JGadget::TNodeLinkList::iterator *probe_112 = 0;
JGadget::TSingleLinkListNode *probe_114 = 0;
JGadget::TSingleNodeLinkList *probe_115 = 0;
JGadget::TSingleNodeLinkList::iterator *probe_116 = 0;
JGadget::TVector_pointer_void *probe_117 = 0;
JKRArcFinder *probe_130 = 0;
JKRArchive *probe_131 = 0;
JKRArchive::SDIFileEntry *probe_132 = 0;
JKRDisposer *probe_136 = 0;
JKRExpHeap *probe_140 = 0;
JKRExpHeap::CMemBlock *probe_141 = 0;
JKRFileFinder *probe_143 = 0;
JKRFileLoader *probe_144 = 0;
JKRHeap *probe_145 = 0;
JKRHeap::TState *probe_146 = 0;
JKRMemArchive *probe_147 = 0;
JKRSolidHeap *probe_148 = 0;
JKRStdHeap *probe_149 = 0;
JPABaseEmitter *probe_151 = 0;
JPABaseParticle *probe_152 = 0;
JPADataBlock *probe_153 = 0;
JPADefaultTexture *probe_154 = 0;
JPAEmitterData *probe_155 = 0;
JPAResourceManager *probe_156 = 0;
JPATexture *probe_157 = 0;
JPATextureResource *probe_158 = 0;
JSUInputStream *probe_160 = 0;
JSUIosBase *probe_161 = 0;
JSUMemoryInputStream *probe_162 = 0;
JSUMemoryOutputStream *probe_163 = 0;
JSUOutputStream *probe_164 = 0;
JSUPtrLink *probe_165 = 0;
JSUPtrList *probe_166 = 0;
JSURandomInputStream *probe_167 = 0;
JSURandomOutputStream *probe_168 = 0;
JStage::TActor *probe_169 = 0;
JStage::TCamera *probe_171 = 0;
JStage::TObject *probe_172 = 0;
JStage::TSystem *probe_173 = 0;
JUTConsole *probe_174 = 0;
JUTFont *probe_177 = 0;
JUTFont::TWidth *probe_178 = 0;
JUTGamePad *probe_179 = 0;
JUTGamePad::C3ButtonReset *probe_180 = 0;
JUTGamePad::CButton *probe_181 = 0;
JUTGamePad::CRumble *probe_182 = 0;
JUTGamePad::CStick *probe_183 = 0;
JUTNameTab *probe_184 = 0;
JUTPalette *probe_185 = 0;
JUTPoint *probe_186 = 0;
JUTRect *probe_187 = 0;
JUTResFont *probe_188 = 0;
JUTTexture *probe_189 = 0;
JUtility::TColor *probe_190 = 0;
M3UModel *probe_191 = 0;
M3UModelCommon *probe_192 = 0;
M3UModelCommonMario *probe_193 = 0;
M3UModelMario *probe_194 = 0;
M3UMtxCalcSetInfo *probe_195 = 0;
MActor *probe_196 = 0;
MActorAnmBase *probe_197 = 0;
MActorAnmBck *probe_198 = 0;
MActorAnmBlk *probe_199 = 0;
MActorAnmBpk *probe_200 = 0;
MActorAnmBrk *probe_201 = 0;
MActorAnmBtk *probe_202 = 0;
MActorAnmBtp *probe_203 = 0;
MActorAnmData *probe_204 = 0;
MAnmSound *probe_205 = 0;
MSBgm *probe_206 = 0;
MSModBgm *probe_207 = 0;
MSound *probe_208 = 0;
MSoundSESystem::MSoundSE *probe_209 = 0;
ObjData *probe_210 = 0;
ObjPhysicalData *probe_211 = 0;
ObjPhysicalInfo *probe_212 = 0;
ResFONT *probe_213 = 0;
ResNTab *probe_214 = 0;
ResTIMG *probe_215 = 0;
ResTLUT *probe_216 = 0;
RumbleMgr *probe_217 = 0;
SDLDrawBufToken *probe_218 = 0;
SDLModel *probe_219 = 0;
SDLModelData *probe_220 = 0;
TAlphaShadowQuad *probe_222 = 0;
TApplication *probe_223 = 0;
TBGCheckData *probe_224 = 0;
TBGCheckList *probe_225 = 0;
TBGCheckListRoot *probe_226 = 0;
TBGCheckListWarp *probe_227 = 0;
TBGPolDrop *probe_228 = 0;
TBGWallCheckRecord *probe_229 = 0;
TBaseNPC *probe_234 = 0;
TBaseParam *probe_235 = 0;
TBossGesso *probe_236 = 0;
TBossGessoManager *probe_237 = 0;
TBossManta *probe_238 = 0;
TBoundPane *probe_242 = 0;
TCamConnecter *probe_243 = 0;
TCamParamData *probe_244 = 0;
TCamSaveKindParam *probe_245 = 0;
TCameraInbetween *probe_246 = 0;
TCameraKindParam *probe_247 = 0;
TCameraMapTool *probe_248 = 0;
TCameraMarioData *probe_249 = 0;
TCameraShake *probe_250 = 0;
TCardBookmarkInfo *probe_251 = 0;
TCardBookmarkInfo::TBlockData *probe_252 = 0;
TCardLoad *probe_253 = 0;
TCardManager *probe_254 = 0;
TCircleShadowRequest *probe_255 = 0;
TCoin *probe_256 = 0;
TConductor *probe_257 = 0;
TConductor::TConductorPacket *probe_258 = 0;
TConductor::TConductorParams *probe_259 = 0;
TConsoleStr *probe_260 = 0;
TCoord2D *probe_261 = 0;
TCubeCameraInfo *probe_262 = 0;
TCubeGeneralInfo *probe_263 = 0;
TCubeManagerArea *probe_264 = 0;
TCubeManagerBase *probe_265 = 0;
TCubeManagerFast *probe_266 = 0;
TCubeManagerMarioIn *probe_267 = 0;
TDemoCannon *probe_268 = 0;
TDrawSyncCallback *probe_271 = 0;
TEffectObjBase *probe_272 = 0;
TEffectObjManager *probe_273 = 0;
TEggYoshi *probe_274 = 0;
TEnemyManager *probe_275 = 0;
TEnemyMario *probe_276 = 0;
TEventWatcher *probe_277 = 0;
TExPane *probe_278 = 0;
TFlagManager *probe_279 = 0;
TGCConsole2 *probe_280 = 0;
TGCConsole2::HealthPoint *probe_281 = 0;
TGCLogoDir *probe_282 = 0;
TGameSequence *probe_283 = 0;
TGraphGroup *probe_284 = 0;
TGraphNode *probe_285 = 0;
TGraphTracer *probe_286 = 0;
TGraphWeb *probe_287 = 0;
TGuide *probe_288 = 0;
THamuKuri *probe_289 = 0;
THamuKuriManager *probe_290 = 0;
THamuKuriSaveLoadParams *probe_291 = 0;
THitActor *probe_292 = 0;
TIdxGroupObj *probe_293 = 0;
TItem *probe_294 = 0;
TItemManager *probe_295 = 0;
TJointModel *probe_296 = 0;
TJointModelManager *probe_297 = 0;
TJointObj *probe_298 = 0;
TKukkuBall *probe_299 = 0;
TLensFlare *probe_300 = 0;
TLensGlow *probe_301 = 0;
TLightCommon *probe_302 = 0;
TLiveActor *probe_303 = 0;
TLiveManager *probe_304 = 0;
TLodAnm *probe_305 = 0;
TLodAnmIndex *probe_306 = 0;
TMActorKeeper *probe_307 = 0;
TMBindShadowBody *probe_308 = 0;
TMBindShadowManager *probe_309 = 0;
TMap *probe_310 = 0;
TMapCollisionBase *probe_311 = 0;
TMapCollisionData *probe_312 = 0;
TMapCollisionManager *probe_313 = 0;
TMapCollisionMove *probe_314 = 0;
TMapCollisionStatic *probe_315 = 0;
TMapCollisionWarp *probe_316 = 0;
TMapObjBall *probe_321 = 0;
TMapObjBase *probe_322 = 0;
TMapObjBaseManager *probe_323 = 0;
TMapObjGeneral *probe_324 = 0;
TMapObjTree *probe_325 = 0;
TMapObjWave *probe_326 = 0;
TMapWarp *probe_327 = 0;
TMapWarp::TMapWarpInfo *probe_328 = 0;
TMapXlu *probe_329 = 0;
TMarDirector *probe_330 = 0;
TMarNameRefGen *probe_331 = 0;
TMario *probe_332 = 0;
TMario::JumpSlipRecord *probe_333 = 0;
TMario::TAttackParams *probe_334 = 0;
TMario::TBodyAngleParams *probe_335 = 0;
TMario::TClimbParams *probe_336 = 0;
TMario::TControllerParams *probe_337 = 0;
TMario::TDeParams *probe_338 = 0;
TMario::TDemoParams *probe_339 = 0;
TMario::TDirtyParams *probe_340 = 0;
TMario::TDivingParams *probe_341 = 0;
TMario::TDmgEnemyParams *probe_342 = 0;
TMario::TEParams *probe_343 = 0;
TMario::TEffectParams *probe_344 = 0;
TMario::TGraffitoParams *probe_345 = 0;
TMario::THangFenceParams *probe_346 = 0;
TMario::THangRoofParams *probe_347 = 0;
TMario::THoverParams *probe_348 = 0;
TMario::TJumpParams *probe_349 = 0;
TMario::TMotorParams *probe_350 = 0;
TMario::TOptionParams *probe_351 = 0;
TMario::TParticleParams *probe_352 = 0;
TMario::TPullParams *probe_353 = 0;
TMario::TRunParams *probe_354 = 0;
TMario::TSlipParams *probe_355 = 0;
TMario::TSoundParams *probe_356 = 0;
TMario::TSurfingParams *probe_357 = 0;
TMario::TSwimParams *probe_358 = 0;
TMario::TUpperBodyParams *probe_359 = 0;
TMario::TWaterEffectParams *probe_360 = 0;
TMario::TWireParams *probe_361 = 0;
TMario::TYoshiParams *probe_362 = 0;
TMarioAnimeData *probe_363 = 0;
TMarioCap *probe_364 = 0;
TMarioControllerWork *probe_365 = 0;
TMarioGamePad *probe_366 = 0;
TMarioParticleManager *probe_367 = 0;
TMarioParticleManager::TInfo *probe_368 = 0;
TMarioSoundValues *probe_369 = 0;
TMenuDirector *probe_370 = 0;
TModelDataLoadEntry *probe_371 = 0;
TModelWaterManager *probe_372 = 0;
TMovieDirector *probe_373 = 0;
TMushroom1up *probe_374 = 0;
TNintendo2D *probe_379 = 0;
TNormalLift *probe_380 = 0;
TNozzleBase *probe_381 = 0;
TNozzleBase::TEmitParams *probe_382 = 0;
TNozzleBox *probe_383 = 0;
TNozzleDeform *probe_384 = 0;
TNozzleTrigger *probe_385 = 0;
TObjChara *probe_386 = 0;
TObjCheckList *probe_387 = 0;
TObjHitCheck *probe_388 = 0;
TObjManager *probe_389 = 0;
TOptionControl *probe_390 = 0;
TOptionRumbleUnit *probe_391 = 0;
TOptionSoundUnit *probe_392 = 0;
TOptionSubtitleUnit *probe_393 = 0;
TParams *probe_394 = 0;
TPerformList *probe_395 = 0;
TPollutionLayer *probe_396 = 0;
TPollutionManager *probe_397 = 0;
TPollutionManager::TPollutionInfo *probe_398 = 0;
TPollutionObj *probe_399 = 0;
TPollutionPos *probe_400 = 0;
TRailBlock *probe_401 = 0;
TRailMapObj *probe_402 = 0;
TRailNode *probe_403 = 0;
TResetFruit *probe_404 = 0;
TRidingInfo *probe_405 = 0;
TSMSFader *probe_406 = 0;
TSMSFader::WipeRequest *probe_407 = 0;
TScenarioArchiveName *probe_408 = 0;
TSelectDir *probe_409 = 0;
TSelectGrad *probe_410 = 0;
TSelectMenu *probe_411 = 0;
TSelectShine *probe_412 = 0;
TSelectShineManager *probe_413 = 0;
TSharedMActorSet *probe_414 = 0;
TShine *probe_415 = 0;
TShineFader *probe_416 = 0;
TSmallEnemy *probe_417 = 0;
TSmallEnemyManager *probe_418 = 0;
TSmallEnemyParams *probe_419 = 0;
TSmplFader *probe_420 = 0;
TSpcBinary *probe_421 = 0;
TSpcInterp *probe_422 = 0;
TSpcSlice *probe_423 = 0;
TSpineEnemy *probe_424 = 0;
TSpineEnemyParams *probe_425 = 0;
TSplinePath *probe_426 = 0;
TSplineRail *probe_427 = 0;
TStrategy *probe_428 = 0;
TSunGlass *probe_429 = 0;
TSunModel *probe_430 = 0;
TTakeActor *probe_431 = 0;
TTargetArrow *probe_432 = 0;
TWalkerEnemy *probe_433 = 0;
TWalkerEnemyParams *probe_434 = 0;
TWaterGun *probe_436 = 0;
TWoodBlock *probe_437 = 0;
TYoshi *probe_438 = 0;
Talk2D2 *probe_439 = 0;
anim_data *probe_440 = 0;
hit_data *probe_441 = 0;
map_col_data *probe_442 = 0;
map_col_info *probe_443 = 0;
obj_hit_info *probe_444 = 0;
obj_info *probe_445 = 0;
sink_data *probe_446 = 0;
sound_data *probe_447 = 0;

TBossPakkunParams *probe_1000 = 0;
TBossPakkun *probe_1001 = 0;
TNerveBPWait *probe_1002 = 0;
TNerveBPTakeOff *probe_1003 = 0;
TNerveBPFly *probe_1004 = 0;
TNerveBPHover *probe_1005 = 0;
TBPHeadHit *probe_1006 = 0;
TBPNavel *probe_1007 = 0;
TBPPolDrop *probe_1008 = 0;
TBPTornado *probe_1009 = 0;
TBossPakkunManager *probe_1010 = 0;
