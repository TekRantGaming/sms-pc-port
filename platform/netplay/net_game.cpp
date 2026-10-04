// Online co-op, game side: publishes the local Mario's pose and draws the
// other players' Marios. Built into the game library with the game's flags,
// since it uses the decomp's classes; reached from TMario::perform
// (decomp-patches/zzz-pc-netplay.patch) for the local Mario only.
//
// A remote Mario is a puppet: a J3DModel of the local Mario's own body,
// hand and cap model data, posed every frame from the remote player's
// NetPose (the base matrix after calcBaseMtx, both animation layers with
// their frames and blend, and the face pattern). It runs no game logic,
// so nothing else in the stage knows it is there. The models are allocated
// on the game's current heap when first needed in a stage and dropped with
// it: a new local Mario means a new stage.
#include <Player/Mario.hpp>
#include <Player/MarioCap.hpp>
#include <Player/WaterGun.hpp>
#include <Player/NozzleBase.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <M3DUtil/M3UJoint.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <MarioUtil/PacketUtil.hpp>
#include <System/Application.hpp>
#include <Camera/Camera.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "netplay.h"
#include <sms_gx/gx_pc.h> // name tags over the presented frame

extern "C" void port_log(const char* fmt, ...);


namespace {

const u32 kPartFlags = J3DMLF_MaterialPEFull | (16 << J3DMLF_TevStageNumShift);

struct Puppet {
	TMario* owner; // the local Mario whose model data it shares (its stage)
	J3DModel* body;
	M3UModelMario* model;
	J3DFrameCtrl* frames;
	SomeModelMarioStruct* layers;
	M3UModel::Unk1CStruct* tex;
	J3DModel* hands[5];
	J3DModel* cap;
	J3DModel* fludd;        // FLUDD, on the chest
	J3DModel* nozzles[6];   // its nozzles, built when first worn
	int nozzle;             // the one drawn this frame, or -1
	bool fluddOn;           // FLUDD is drawn this frame
	char name[NET_NAME_LEN];
	bool drawn; // posed this frame
};

const int kNozzles = 6;

// The model of the local Mario's FLUDD nozzle `i`, or null (the spray
// nozzle is part of FLUDD's own model).
J3DModel* nozzleModel(TMario* m, int i)
{
	if (!m->mWaterGun || i < 0 || i >= kNozzles || !m->mWaterGun->getNozzle(u8(i)))
		return 0;
	MActor* actor = m->mWaterGun->getNozzle(u8(i))->getMActor();
	return actor ? actor->getModel() : 0;
}

Puppet sPuppets[NET_MAX_PLAYERS];
TMario* sStageMario;
int sDebug = -1; // SMS_NET_DEBUG
int sFrame;

J3DModel* copyModel(J3DModel* from, u32 flags)
{
	return from ? new J3DModel(from->getModelData(), flags, 1) : 0;
}

// Builds a puppet from the local Mario's model data, as TMario::initModel
// builds his own model, but with its own frame controls and layer state.
bool build(Puppet& p, TMario* m)
{
	memset(&p, 0, sizeof p);
	M3UModelMario* src = m->getM3UModel();
	if (!src || !m->mBodyModelData)
		return false;
	p.owner = m;
	p.body  = new J3DModel(m->mBodyModelData, 0, 1);
	p.model = new M3UModelMario();
	p.model->unk8  = p.body;
	p.model->unk4  = src->unk4;
	p.model->unk20 = src->unk20;
	p.frames       = new J3DFrameCtrl[3];
	p.model->unkC  = p.frames;
	p.layers       = new SomeModelMarioStruct[2];
	p.layers[0]    = src->unk24[0];
	p.layers[1]    = src->unk24[1];
	p.model->unk10 = 2;
	p.model->unk24 = p.layers;
	if (src->unk1C) {
		p.tex          = new M3UModel::Unk1CStruct;
		*p.tex         = *src->unk1C;
		p.model->unk1C = p.tex;
	}
	for (int i = 0; i < 3; i++)
		p.frames[i].setRate(0.0f); // frames come from the pose

	// as TMario::finalDrawInitialize
	SMS_MakeDLAndLock(p.body);
	for (int i = 0; i < m->mBodyModelData->getMaterialNum(); ++i)
		if (i == m->mMaterialIdEyeL || i == m->mMaterialIdEyeR)
			p.body->getMatPacketArray()[i].offFlag(0x1);
	for (int i = 0; i < m->mBodyModelData->getMaterialNum(); ++i)
		SMS_InitPacket_OneTevKColorAndFog(p.body, i, GX_KCOLOR0, nullptr);

	p.hands[0] = copyModel(m->mHandModels[0][0], kPartFlags);
	p.hands[1] = copyModel(m->mHandModels[0][1], kPartFlags);
	p.hands[2] = copyModel(m->mHandModels[1][0], kPartFlags);
	p.hands[3] = copyModel(m->mHandModels[1][1], kPartFlags);
	p.hands[4] = copyModel(m->mRHand4ndModel, kPartFlags);
	if (m->mCap && m->mCap->unk10[0])
		p.cap = copyModel(m->mCap->unk10[0], kPartFlags);
	if (m->mWaterGun)
		p.fludd = copyModel(m->mWaterGun->getModel(), 0);
	p.nozzle = -1;
	return true;
}

void capturePose(TMario* m, NetPose& pose)
{
	memset(&pose, 0, sizeof pose);
	M3UModelMario* src = m->getM3UModel();
	MtxPtr base        = src->unk8->getBaseTRMtx();
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			pose.mtx[r][c] = base[r][c];
	for (int l = 0; l < 2; l++) {
		pose.anm[l][0] = src->unk24[l].unk4[0];
		pose.anm[l][1] = src->unk24[l].unk4[1];
		pose.blend[l]  = src->unk20->unk18[src->unk24[l].unk3 == 0xff ? 0 : src->unk24[l].unk3].mMotionBlendRatio;
	}
	for (int i = 0; i < 3; i++)
		pose.frame[i] = src->unkC[i].getFrame();
	pose.texPattern = src->unk1C ? src->unk1C->unk0 : 0xff;
	pose.flags      = NET_POSE_IN_STAGE;
	if ((m->unk114 & TMario::UNK114_FLAG_VISIBLE) && !m->checkFlag(MARIO_FLAG_UNK4))
		pose.flags |= NET_POSE_VISIBLE;
	if (m->mCap && m->mCap->unkC == m->mCap->unk10[0])
		pose.flags |= NET_POSE_CAP;
	pose.nozzle = 0xff;
	if (m->checkFlag(MARIO_FLAG_HAS_FLUDD) && m->mWaterGun)
		pose.nozzle = u8(m->mWaterGun->getCurrentNozzleIndex());
	pose.area    = gpApplication.mCurrArea.getStage();
	pose.episode = gpApplication.mCurrArea.getScenario();
}

// Poses and animates a puppet (CUE_CALC_ANIM).
void calcPuppet(Puppet& p, const NetPose& pose, JDrama::TGraphics* graphics)
{
	M3UModelMario* model = p.model;
	for (int l = 0; l < 2; l++) {
		SomeModelMarioStruct& layer = p.layers[l];
		const u16 maxAnm            = 199;
		if (pose.anm[l][0] >= maxAnm || pose.anm[l][1] >= maxAnm)
			continue;
		if (layer.unk4[0] != pose.anm[l][0] && model->unk4->unk4[pose.anm[l][0]])
			p.frames[layer.unk8].init(model->unk4->unk4[pose.anm[l][0]]->getFrameMax());
		layer.unk4[0] = pose.anm[l][0];
		layer.unk4[1] = pose.anm[l][1];
	}
	for (int i = 0; i < 3; i++) {
		p.frames[i].setRate(0.0f);
		p.frames[i].setFrame(pose.frame[i]);
	}
	if (p.tex)
		p.tex->unk0 = pose.texPattern;

	Mtx base;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++)
			base[r][c] = pose.mtx[r][c];
	MTXCopy(base, p.body->getBaseTRMtx());

	// The blend calculators are shared with the local Mario: set the remote's
	// ratios around its calc, then put his back.
	M3UMtxCalcSIAnmBlendQuat* calc = model->unk20->unk18;
	const f32 keep0 = calc[0].mMotionBlendRatio, keep1 = calc[1].mMotionBlendRatio;
	calc[0].mMotionBlendRatio = pose.blend[0];
	calc[1].mMotionBlendRatio = pose.blend[1];
	model->perform(CUE_CALC_ANIM, graphics);
	calc[0].mMotionBlendRatio = keep0;
	calc[1].mMotionBlendRatio = keep1;

	TMario* m = p.owner;
	MtxPtr handR = p.body->getAnmMtx(m->mJointIdHandR);
	MtxPtr handL = p.body->getAnmMtx(m->mJointIdHandL);
	for (int i = 0; i < 5; i++) {
		if (!p.hands[i])
			continue;
		p.hands[i]->setBaseTRMtx((i == 1 || i == 3) ? handL : handR);
		p.hands[i]->calc();
	}
	if (p.cap) {
		p.cap->setBaseTRMtx(p.body->getAnmMtx(m->mJointIdMHead));
		p.cap->calc();
	}

	// FLUDD on the chest, as TMario::perform places it, and its nozzle on
	// FLUDD's nozzle joint, as TWaterGun::perform does
	p.nozzle  = -1;
	p.fluddOn = false;
	if (p.fludd && pose.nozzle != 0xff && m->mWaterGun) {
		p.fludd->setBaseTRMtx(p.body->getAnmMtx(m->mJointIdChest));
		p.fludd->calc();
		p.fluddOn = true;
		const int n = pose.nozzle;
		if (n < kNozzles && nozzleModel(m, n)) {
			if (!p.nozzles[n])
				p.nozzles[n] = copyModel(nozzleModel(m, n), 0);
			p.nozzles[n]->setBaseTRMtx(p.fludd->getAnmMtx(m->mWaterGun->unk1CD8));
			p.nozzles[n]->calc();
			p.nozzle = n;
		}
	}
}

bool sameStage(const NetPose& pose)
{
	return (pose.flags & NET_POSE_IN_STAGE) && (pose.flags & NET_POSE_VISIBLE)
	       && pose.area == gpApplication.mCurrArea.getStage()
	       && pose.episode == gpApplication.mCurrArea.getScenario();
}

} // namespace

// stage 0: after the local Mario's calcAnim (CUE_MOVE); 1: after calcView;
// 2: after entryModels.
extern "C" void port_net_mario(TMario* m, int stage, JDrama::TGraphics* graphics)
{
	if (!port_net_active())
		return;
	if (m != sStageMario) { // a new stage: the old puppets went with its heap
		sStageMario = m;
		memset(sPuppets, 0, sizeof sPuppets);
	}
	if (stage == 0) {
		if (sDebug < 0)
			sDebug = getenv("SMS_NET_DEBUG") && *getenv("SMS_NET_DEBUG") == '1';
		sFrame++;
		NetPose pose;
		capturePose(m, pose);
		port_net_publish(&pose);
		for (int s = 0; s < NET_MAX_PLAYERS; s++) {
			Puppet& p = sPuppets[s];
			p.drawn = false;
			NetRemote r;
			if (!port_net_remote(s, &r) || r.age > 1.0f || !sameStage(r.pose))
				continue;
			if (!p.body) {
				JKRHeap* heap = JKRHeap::getCurrentHeap();
				port_log("[net] drawing %s here (heap free %u KiB)\n", r.name,
				         heap ? (unsigned)(heap->getFreeSize() / 1024) : 0u);
				if (!build(p, m)) {
					memset(&p, 0, sizeof p);
					continue;
				}
			}
			calcPuppet(p, r.pose, graphics);
			p.drawn = true;
			memcpy(p.name, r.name, NET_NAME_LEN);
			// SMS_NET_DEBUG=1: where each remote Mario is, every 2 s
			if (sDebug && sFrame % 120 == 0) {
				const f32 dx = r.pose.mtx[0][3] - m->mPosition.x, dz = r.pose.mtx[2][3] - m->mPosition.z;
				port_log("[net] %s at (%.0f, %.0f, %.0f), %.0f units from you, anim %u frame %.1f\n", r.name,
				         r.pose.mtx[0][3], r.pose.mtx[1][3], r.pose.mtx[2][3], sqrtf(dx * dx + dz * dz),
				         r.pose.anm[0][0], r.pose.frame[0]);
			}
		}
	} else if (stage == 1) {
		// name tags: each drawn Mario's head, a little above it, through the
		// game camera's view and projection
		GXPCNameTag tags[NET_MAX_PLAYERS];
		int tagCount = 0;
		const f32 fovy   = gpCamera ? gpCamera->JSGGetProjectionFovy() : 50.0f;
		const f32 aspect = gpCamera ? gpCamera->JSGGetProjectionAspect() : 4.0f / 3.0f;
		const f32 t      = tanf(fovy * 0.5f * 3.14159265f / 180.0f);
		for (int s = 0; s < NET_MAX_PLAYERS; s++) {
			Puppet& p = sPuppets[s];
			if (!p.drawn)
				continue;
			MtxPtr head = p.body->getAnmMtx(m->mJointIdHead);
			Vec world   = { head[0][3], head[1][3] + 70.0f, head[2][3] };
			Vec view;
			PSMTXMultVec(graphics->mViewMtx, &world, &view);
			if (view.z > -10.0f || view.z < -8000.0f) // behind the camera, or far away
				continue;
			GXPCNameTag& tag = tags[tagCount++];
			tag.x = (view.x / -view.z) / (t * aspect);
			tag.y = (view.y / -view.z) / t;
			memcpy(tag.name, p.name, sizeof tag.name);
			tag.name[sizeof tag.name - 1] = 0;
		}
		GXPC_SetNameTags(tags, tagCount);
		for (int s = 0; s < NET_MAX_PLAYERS; s++) {
			Puppet& p = sPuppets[s];
			if (!p.drawn)
				continue;
			p.body->viewCalc();
			for (int i = 0; i < 5; i++)
				if (p.hands[i])
					p.hands[i]->viewCalc();
			if (p.cap)
				p.cap->viewCalc();
			if (p.fluddOn)
				p.fludd->viewCalc();
			if (p.nozzle >= 0)
				p.nozzles[p.nozzle]->viewCalc();
		}
	} else if (stage == 2) {
		for (int s = 0; s < NET_MAX_PLAYERS; s++) {
			Puppet& p = sPuppets[s];
			if (!p.drawn)
				continue;
			p.model->perform(CUE_ENTRY, graphics);
			for (int i = 0; i < 5; i++)
				if (p.hands[i])
					p.hands[i]->entry();
			if (p.cap)
				p.cap->entry();
			if (p.fluddOn)
				p.fludd->entry();
			if (p.nozzle >= 0)
				p.nozzles[p.nozzle]->entry();
		}
	}
}
