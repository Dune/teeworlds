#include "duck_bridge.h"
#include "duck_js.h"

#include <game/client/animstate.h>
#include <game/client/render.h>
#include <game/client/components/skins.h>
#include <game/client/components/controls.h>

#include <engine/external/zlib/zlib.h>
#include <engine/external/pnglite/pnglite.h>
#include <engine/storage.h>
#include <engine/shared/network.h>
#include <engine/shared/growbuffer.h>
#include <engine/shared/config.h>

#include <base/hash.h>
#include <zip.h>

CMultiStackAllocator::CMultiStackAllocator()
{
	CStackBuffer StackBuffer;
	StackBuffer.m_pBuffer = (char*)mem_alloc(STACK_BUFFER_CAPACITY, 1);
	StackBuffer.m_Cursor = 0;

	m_aStacks.hint_size(8);
	m_aStacks.add(StackBuffer);
	m_CurrentStack = 0;
}

CMultiStackAllocator::~CMultiStackAllocator()
{
	const int StackBufferCount = m_aStacks.size();
	for(int s = 0; s < StackBufferCount; s++)
	{
		mem_free(m_aStacks[s].m_pBuffer);
	}
}

void* CMultiStackAllocator::Alloc(int Size)
{
	dbg_assert(Size <= STACK_BUFFER_CAPACITY, "Trying to alloc a large buffer");

	// if current stack is not full, alloc from it
	CStackBuffer& CurrentSb = m_aStacks[m_CurrentStack];
	if(CurrentSb.m_Cursor + Size <= STACK_BUFFER_CAPACITY)
	{
		int MemBlockStart = CurrentSb.m_Cursor;
		CurrentSb.m_Cursor += Size;
		return CurrentSb.m_pBuffer + MemBlockStart;
	}

	// else add a new stack if needed
	if(m_CurrentStack+1 >= m_aStacks.size())
	{
		CStackBuffer StackBuffer;
		StackBuffer.m_pBuffer = (char*)mem_alloc(STACK_BUFFER_CAPACITY, 1);
		StackBuffer.m_Cursor = 0;
		m_aStacks.add(StackBuffer);
	}

	// and try again
	m_CurrentStack++;
	return Alloc(Size);
}

void CMultiStackAllocator::Clear()
{
	const int StackBufferCount = m_aStacks.size();
	for(int s = 0; s < StackBufferCount; s++)
	{
		m_aStacks[s].m_Cursor = 0;
	}
	m_CurrentStack = 0;
}


void CDuckBridge::DrawTeeBodyAndFeet(const CTeeDrawBodyAndFeetInfo& TeeDrawInfo, const CTeeSkinInfo& SkinInfo)
{
	CAnimState State;
	State.Set(&g_pData->m_aAnimations[ANIM_BASE], 0);

	CTeeRenderInfo RenderInfo;
	mem_move(RenderInfo.m_aTextures, SkinInfo.m_aTextures, sizeof(RenderInfo.m_aTextures));
	mem_move(RenderInfo.m_aColors, SkinInfo.m_aColors, sizeof(RenderInfo.m_aColors));
	RenderInfo.m_Size = TeeDrawInfo.m_Size;
	RenderInfo.m_GotAirJump = TeeDrawInfo.m_GotAirJump;

	vec2 Direction = direction(TeeDrawInfo.m_Angle);
	vec2 Pos = vec2(TeeDrawInfo.m_Pos[0], TeeDrawInfo.m_Pos[1]);
	int Emote = TeeDrawInfo.m_Emote;

	const float WalkTimeMagic = 100.0f;
	float WalkTime =
		((Pos.x >= 0)
			? fmod(Pos.x, WalkTimeMagic)
			: WalkTimeMagic - fmod(-Pos.x, WalkTimeMagic))
		/ WalkTimeMagic;

	if(TeeDrawInfo.m_IsWalking)
		State.Add(&g_pData->m_aAnimations[ANIM_WALK], WalkTime, 1.0f);
	else
	{
		if(TeeDrawInfo.m_IsGrounded)
			State.Add(&g_pData->m_aAnimations[ANIM_IDLE], 0, 1.0f); // TODO: some sort of time here
		else
			State.Add(&g_pData->m_aAnimations[ANIM_INAIR], 0, 1.0f); // TODO: some sort of time here
	}

	RenderTools()->RenderTee(&State, &RenderInfo, Emote, Direction, Pos);
}

void CDuckBridge::DrawTeeHand(const CDuckBridge::CTeeDrawHand& Hand, const CTeeSkinInfo& SkinInfo)
{
	CTeeRenderInfo RenderInfo;
	mem_move(RenderInfo.m_aTextures, SkinInfo.m_aTextures, sizeof(RenderInfo.m_aTextures));
	mem_move(RenderInfo.m_aColors, SkinInfo.m_aColors, sizeof(RenderInfo.m_aColors));
	RenderInfo.m_Size = Hand.m_Size;
	vec2 Pos(Hand.m_Pos[0], Hand.m_Pos[1]);
	vec2 Offset(Hand.m_Offset[0], Hand.m_Offset[1]);
	vec2 Dir = direction(Hand.m_AngleDir);

	RenderTools()->RenderTeeHand(&RenderInfo, Pos, Dir, Hand.m_AngleOff, Offset);
}

void CDuckBridge::Init(CDuckJs* pDuckJs)
{

}

void CDuckBridge::Reset()
{
	for(int i = 0 ; i < m_aTextures.size(); i++)
	{
		Graphics()->UnloadTexture(&m_aTextures[i].m_Handle);
	}

	m_aTextures.clear();
	m_HudPartsShown = CHudPartsShown();
	m_CurrentPacketFlags = -1;

	// unload skin parts
	CSkins* pSkins = m_pClient->m_pSkins;
	for(int i = 0; i < m_aSkinPartsToUnload.size(); i++)
	{
		const CSkinPartName& spn = m_aSkinPartsToUnload[i];
		int Index = pSkins->FindSkinPart(spn.m_Type, spn.m_aName, false);
		if(Index != -1)
			pSkins->RemoveSkinPart(spn.m_Type, Index);
	}
	m_aSkinPartsToUnload.clear();
	m_Collision.Reset();
}

void CDuckBridge::QueueSetColor(const float* pColor)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_COLOR;
	mem_move(Cmd.m_Color, pColor, sizeof(Cmd.m_Color));
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetTexture(int TextureID)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_TEXTURE;
	Cmd.m_TextureID = TextureID;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetQuadSubSet(const float* pSubSet)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_QUAD_SUBSET;
	mem_move(Cmd.m_QuadSubSet, pSubSet, sizeof(Cmd.m_QuadSubSet));
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetQuadRotation(float Angle)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_QUAD_ROTATION;
	Cmd.m_QuadRotation = Angle;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetTeeSkin(const CTeeSkinInfo& SkinInfo)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_TEE_SKIN;
	Cmd.m_TeeSkinInfo = SkinInfo;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueSetFreeform(const IGraphics::CFreeformItem *pFreeform, int FreeformCount)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::SET_FREEFORM_VERTICES;
	Cmd.m_pFreeformQuads = (float*)pFreeform;
	Cmd.m_FreeformQuadCount = FreeformCount;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueDrawFreeform(vec2 Pos)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CRenderCmd::DRAW_FREEFORM;
	mem_move(Cmd.m_FreeformPos, &Pos, sizeof(float)*2);
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::SetHudPartsShown(CHudPartsShown hps)
{
	m_HudPartsShown = hps;
}

void CDuckBridge::QueueDrawQuad(IGraphics::CQuadItem Quad)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CDuckBridge::CRenderCmd::DRAW_QUAD;
	mem_move(Cmd.m_Quad, &Quad, sizeof(Cmd.m_Quad)); // yep, this is because we don't have c++11
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueDrawQuadCentered(IGraphics::CQuadItem Quad)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CDuckBridge::CRenderCmd::DRAW_QUAD_CENTERED;
	mem_move(Cmd.m_Quad, &Quad, sizeof(Cmd.m_Quad));
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueDrawTeeBodyAndFeet(const CTeeDrawBodyAndFeetInfo& TeeDrawInfo)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CDuckBridge::CRenderCmd::DRAW_TEE_BODYANDFEET;
	Cmd.m_TeeBodyAndFeet = TeeDrawInfo;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

void CDuckBridge::QueueDrawTeeHand(const CDuckBridge::CTeeDrawHand& Hand)
{
	CRenderCmd Cmd;
	Cmd.m_Type = CDuckBridge::CRenderCmd::DRAW_TEE_HAND;
	Cmd.m_TeeHand = Hand;
	m_aRenderCmdList[m_CurrentDrawSpace].add(Cmd);
}

bool CDuckBridge::LoadTexture(const char *pTexturePath, const char* pTextureName)
{
	int Len = str_length(pTextureName);
	if(Len < 5) // .png
		return false;

	IGraphics::CTextureHandle Handle = Graphics()->LoadTexture(pTexturePath, IStorage::TYPE_SAVE, CImageInfo::FORMAT_AUTO, 0);
	uint32_t Hash = hash_fnv1a(pTextureName, Len - 4);
	CTextureHashPair Pair = { Hash, Handle };
	m_aTextures.add(Pair);
	return Handle.IsValid();
}

IGraphics::CTextureHandle CDuckBridge::GetTextureFromName(const char *pTextureName)
{
	const uint32_t SearchHash = hash_fnv1a(pTextureName, str_length(pTextureName));

	const CTextureHashPair* Pairs = m_aTextures.base_ptr();
	const int PairCount = m_aTextures.size();

	for(int i = 0; i < PairCount; i++)
	{
		if(Pairs[i].m_Hash == SearchHash)
			return Pairs[i].m_Handle;
	}

	return IGraphics::CTextureHandle();
}

void CDuckBridge::PacketCreate(int NetID, int Flags)
{
	NetID = max(NetID, 0);
	// manual contructor here needed
	m_CurrentPacket.Reset();
	m_CurrentPacket.AddInt((NETMSG_DUCK_NETOBJ) << 1 | 1);
	m_CurrentPacket.AddInt(NetID);

	m_CurrentPacketFlags = Flags;
	dbg_assert(m_CurrentPacket.Size() < 16 && m_CurrentPacket.Size() > 0, "Hmm");
}

void CDuckBridge::PacketPackFloat(float f)
{
	if(m_CurrentPacketFlags == -1) {
		dbg_msg("duck", "ERROR: can't add float to undefined packet");
		return;
	}

	m_CurrentPacket.AddInt(f * 256);
}

void CDuckBridge::PacketPackInt(int i)
{
	if(m_CurrentPacketFlags == -1) {
		dbg_msg("duck", "ERROR: can't add int to undefined packet");
		return;
	}

	m_CurrentPacket.AddInt(i);
}

void CDuckBridge::PacketPackString(const char *pStr, int SizeLimit)
{
	if(m_CurrentPacketFlags == -1) {
		dbg_msg("duck", "ERROR: can't add string to undefined packet");
		return;
	}

	SizeLimit = clamp(SizeLimit, 0, 1024); // reasonable limits
	m_CurrentPacket.AddString(pStr, SizeLimit);
}

void CDuckBridge::SendPacket()
{
	if(m_CurrentPacketFlags == -1) {
		dbg_msg("duck", "ERROR: can't send undefined packet");
		return;
	}

	Client()->SendMsg(&m_CurrentPacket, m_CurrentPacketFlags);
	m_CurrentPacket.Reset();
	m_CurrentPacketFlags = -1;
}

void CDuckBridge::AddSkinPart(const char *pPart, const char *pName, IGraphics::CTextureHandle Handle)
{
	int Type = -1;
	if(str_comp(pPart, "body") == 0) {
		Type = SKINPART_BODY;
	}
	else if(str_comp(pPart, "marking") == 0) {
		Type = SKINPART_MARKING;
	}
	else if(str_comp(pPart, "decoration") == 0) {
		Type = SKINPART_MARKING;
	}
	else if(str_comp(pPart, "hands") == 0) {
		Type = SKINPART_HANDS;
	}
	else if(str_comp(pPart, "feet") == 0) {
		Type = SKINPART_FEET;
	}
	else if(str_comp(pPart, "eyes") == 0) {
		Type = SKINPART_EYES;
	}

	if(Type == -1) {
		return; // part type not found
	}

	CSkins::CSkinPart SkinPart;
	SkinPart.m_OrgTexture = Handle;
	SkinPart.m_ColorTexture = Handle;
	SkinPart.m_BloodColor = vec3(1.0f, 0.0f, 0.0f);
	SkinPart.m_Flags = 0;

	char aPartName[256];
	str_truncate(aPartName, sizeof(aPartName), pName, str_length(pName) - 4);
	str_copy(SkinPart.m_aName, aPartName, sizeof(SkinPart.m_aName));

	m_pClient->m_pSkins->AddSkinPart(Type, SkinPart);

	CSkinPartName SkinPartName;
	str_copy(SkinPartName.m_aName, SkinPart.m_aName, sizeof(SkinPartName.m_aName));
	SkinPartName.m_Type = Type;
	m_aSkinPartsToUnload.add(SkinPartName);
	// FIXME: breaks loaded "skins" (invalidates indexes)
}

void CDuckBridge::AddWeapon(const CWeaponCustomJs &WcJs)
{
	if(WcJs.WeaponID < NUM_WEAPONS || WcJs.WeaponID >= NUM_WEAPONS_DUCK)
	{
		dbg_msg("duck", "ERROR: AddWeapon() :: Weapon ID = %d out of bounds", WcJs.WeaponID);
		return;
	}

	/*const int Count = m_aWeapons.size();
	for(int i = 0; i < Count; i++)
	{
		if(m_aWeapons[i].WeaponID == WcJs.WeaponID)
		{
			dbg_msg("duck", "ERROR: AddWeapon() :: Weapon with ID = %d exists already", WcJs.WeaponID);
			return;
		}
	}*/

	CWeaponCustom WcFind;
	WcFind.WeaponID = WcJs.WeaponID;
	plain_range_sorted<CWeaponCustom> Found = find_binary(m_aWeapons.all(), WcFind);
	if(!Found.empty())
	{
		dbg_msg("duck", "ERROR: AddWeapon() :: Weapon with ID = %d exists already", WcJs.WeaponID);
		// TODO: generate a WARNING?
		return;
	}

	IGraphics::CTextureHandle TexWeaponHandle, TexCursorHandle;

	if(WcJs.aTexWeapon[0])
	{
		TexWeaponHandle = GetTextureFromName(WcJs.aTexWeapon);
		if(!TexWeaponHandle.IsValid())
		{
			dbg_msg("duck", "ERROR: AddWeapon() :: Weapon texture '%s' not found", WcJs.aTexWeapon);
			return;
		}
	}

	if(WcJs.aTexCursor[0])
	{
		TexCursorHandle = GetTextureFromName(WcJs.aTexCursor);
		if(!TexCursorHandle.IsValid())
		{
			dbg_msg("duck", "ERROR: AddWeapon() :: Cursor texture '%s' not found", WcJs.aTexCursor);
			return;
		}
	}


	CWeaponCustom Wc;
	Wc.WeaponID = WcJs.WeaponID;
	Wc.WeaponPos = vec2(WcJs.WeaponX, WcJs.WeaponY);
	Wc.WeaponSize = vec2(WcJs.WeaponSizeX, WcJs.WeaponSizeY);
	Wc.TexWeaponHandle = TexWeaponHandle;
	Wc.TexCursorHandle = TexCursorHandle;
	Wc.HandPos = vec2(WcJs.HandX, WcJs.HandY);
	Wc.HandAngle = WcJs.HandAngle;
	Wc.Recoil = WcJs.Recoil;

	m_aWeapons.add(Wc);
}

CDuckBridge::CWeaponCustom *CDuckBridge::FindWeapon(int WeaponID)
{
	CWeaponCustom WcFind;
	WcFind.WeaponID = WeaponID;
	plain_range_sorted<CWeaponCustom> Found = find_binary(m_aWeapons.all(), WcFind);
	if(Found.empty())
		return 0;

	return &Found.front();
}

void CDuckBridge::RenderDrawSpace(DrawSpace::Enum Space)
{
	const int CmdCount = m_aRenderCmdList[Space].size();
	const CRenderCmd* aCmds = m_aRenderCmdList[Space].base_ptr();

	// TODO: merge CRenderSpace and DrawSpace
	CRenderSpace& RenderSpace = m_aRenderSpace[Space];
	float* pWantColor = RenderSpace.m_aWantColor;
	float* pCurrentColor = RenderSpace.m_aCurrentColor;
	float* pWantQuadSubSet = RenderSpace.m_aWantQuadSubSet;
	float* pCurrentQuadSubSet = RenderSpace.m_aCurrentQuadSubSet;

	for(int i = 0; i < CmdCount; i++)
	{
		const CRenderCmd& Cmd = aCmds[i];

		switch(Cmd.m_Type)
		{
			case CRenderCmd::SET_COLOR: {
				mem_move(pWantColor, Cmd.m_Color, sizeof(Cmd.m_Color));
			} break;

			case CRenderCmd::SET_TEXTURE: {
				RenderSpace.m_WantTextureID = Cmd.m_TextureID;
			} break;

			case CRenderCmd::SET_QUAD_SUBSET: {
				mem_move(pWantQuadSubSet, Cmd.m_QuadSubSet, sizeof(Cmd.m_QuadSubSet));
			} break;

			case CRenderCmd::SET_QUAD_ROTATION: {
				RenderSpace.m_WantQuadRotation = Cmd.m_QuadRotation;
			} break;

			case CRenderCmd::SET_TEE_SKIN: {
				RenderSpace.m_CurrentTeeSkin = Cmd.m_TeeSkinInfo;
			} break;

			case CRenderCmd::SET_FREEFORM_VERTICES: {
				RenderSpace.m_FreeformQuadCount = min(Cmd.m_FreeformQuadCount, CRenderSpace::FREEFORM_MAX_COUNT-1);
				mem_move(RenderSpace.m_aFreeformQuads, Cmd.m_pFreeformQuads, RenderSpace.m_FreeformQuadCount * sizeof(RenderSpace.m_aFreeformQuads[0]));
			} break;

			case CRenderCmd::DRAW_QUAD_CENTERED:
			case CRenderCmd::DRAW_QUAD:
			case CRenderCmd::DRAW_FREEFORM: {
				if(RenderSpace.m_WantTextureID != RenderSpace.m_CurrentTextureID)
				{
					if(RenderSpace.m_WantTextureID < 0)
						Graphics()->TextureClear();
					else
						Graphics()->TextureSet(*(IGraphics::CTextureHandle*)&RenderSpace.m_WantTextureID);
					RenderSpace.m_CurrentTextureID = RenderSpace.m_WantTextureID;
				}
				Graphics()->TextureSet(*(IGraphics::CTextureHandle*)&RenderSpace.m_WantTextureID);

				Graphics()->QuadsBegin();

				if(pWantColor[0] != pCurrentColor[0] ||
				   pWantColor[1] != pCurrentColor[1] ||
				   pWantColor[2] != pCurrentColor[2] ||
				   pWantColor[3] != pCurrentColor[3])
				{
					Graphics()->SetColor(pWantColor[0] * pWantColor[3], pWantColor[1] * pWantColor[3], pWantColor[2] * pWantColor[3], pWantColor[3]);
					mem_move(pCurrentColor, pWantColor, sizeof(float)*4);
				}

				if(pWantQuadSubSet[0] != pCurrentQuadSubSet[0] ||
				   pWantQuadSubSet[1] != pCurrentQuadSubSet[1] ||
				   pWantQuadSubSet[2] != pCurrentQuadSubSet[2] ||
				   pWantQuadSubSet[3] != pCurrentQuadSubSet[3])
				{
					Graphics()->QuadsSetSubset(pWantQuadSubSet[0], pWantQuadSubSet[1], pWantQuadSubSet[2], pWantQuadSubSet[3]);
					mem_move(pCurrentQuadSubSet, pWantQuadSubSet, sizeof(float)*4);
				}

				if(RenderSpace.m_WantQuadRotation != RenderSpace.m_CurrentQuadRotation)
				{
					Graphics()->QuadsSetRotation(RenderSpace.m_WantQuadRotation);
					RenderSpace.m_CurrentQuadRotation = RenderSpace.m_WantQuadRotation;
				}

				if(Cmd.m_Type == CRenderCmd::DRAW_QUAD_CENTERED)
					Graphics()->QuadsDraw((IGraphics::CQuadItem*)&Cmd.m_Quad, 1);
				else if(Cmd.m_Type == CRenderCmd::DRAW_FREEFORM)
				{
					// TODO: is the position even useful here?
					IGraphics::CFreeformItem aTransFreeform[CRenderSpace::FREEFORM_MAX_COUNT];
					const vec2 FfPos(Cmd.m_FreeformPos[0], Cmd.m_FreeformPos[1]);

					// transform freeform object based on position
					for(int f = 0; f < RenderSpace.m_FreeformQuadCount; f++)
					{
						IGraphics::CFreeformItem& ff = aTransFreeform[f];
						IGraphics::CFreeformItem& rff = RenderSpace.m_aFreeformQuads[f];
						ff.m_X0 = rff.m_X0 + FfPos.x;
						ff.m_X1 = rff.m_X1 + FfPos.x;
						ff.m_X2 = rff.m_X2 + FfPos.x;
						ff.m_X3 = rff.m_X3 + FfPos.x;
						ff.m_Y0 = rff.m_Y0 + FfPos.y;
						ff.m_Y1 = rff.m_Y1 + FfPos.y;
						ff.m_Y2 = rff.m_Y2 + FfPos.y;
						ff.m_Y3 = rff.m_Y3 + FfPos.y;
					}
					Graphics()->QuadsDrawFreeform(aTransFreeform, RenderSpace.m_FreeformQuadCount);
				}
				else
					Graphics()->QuadsDrawTL((IGraphics::CQuadItem*)&Cmd.m_Quad, 1);

				Graphics()->QuadsEnd();
			} break;

			case CRenderCmd::DRAW_TEE_BODYANDFEET:
				DrawTeeBodyAndFeet(Cmd.m_TeeBodyAndFeet, RenderSpace.m_CurrentTeeSkin);

				// TODO: do this better
				mem_zero(RenderSpace.m_aWantColor, sizeof(RenderSpace.m_aWantColor));
				mem_zero(RenderSpace.m_aCurrentColor, sizeof(RenderSpace.m_aCurrentColor));
				mem_zero(RenderSpace.m_aWantQuadSubSet, sizeof(RenderSpace.m_aWantQuadSubSet));
				mem_zero(RenderSpace.m_aCurrentQuadSubSet, sizeof(RenderSpace.m_aCurrentQuadSubSet));
				RenderSpace.m_WantTextureID = -1; // clear by default
				RenderSpace.m_CurrentTextureID = 0;
				RenderSpace.m_WantQuadRotation = 0; // clear by default
				RenderSpace.m_CurrentQuadRotation = -1;
				break;

			case CRenderCmd::DRAW_TEE_HAND:
				DrawTeeHand(Cmd.m_TeeHand, RenderSpace.m_CurrentTeeSkin);

				// TODO: do this better
				mem_zero(RenderSpace.m_aWantColor, sizeof(RenderSpace.m_aWantColor));
				mem_zero(RenderSpace.m_aCurrentColor, sizeof(RenderSpace.m_aCurrentColor));
				mem_zero(RenderSpace.m_aWantQuadSubSet, sizeof(RenderSpace.m_aWantQuadSubSet));
				mem_zero(RenderSpace.m_aCurrentQuadSubSet, sizeof(RenderSpace.m_aCurrentQuadSubSet));
				RenderSpace.m_WantTextureID = -1; // clear by default
				RenderSpace.m_CurrentTextureID = 0;
				RenderSpace.m_WantQuadRotation = 0; // clear by default
				RenderSpace.m_CurrentQuadRotation = -1;
				break;

			default:
				dbg_assert(0, "Render command type not handled");
		}
	}

	m_aRenderCmdList[Space].set_size(0);
	RenderSpace = CRenderSpace();
}

void CDuckBridge::CharacterCorePreTick(CCharacterCore** apCharCores)
{
	if(!IsLoaded())
		return;

	duk_context* pCtx = m_Js.Ctx();

	if(!m_Js.GetJsFunction("OnCharacterCorePreTick")) {
		return;
	}

	// arguments (array[CCharacterCore object], array[CNetObj_PlayerInput object])
	duk_idx_t ArrayCharCoresIdx = duk_push_array(pCtx);
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(apCharCores[c])
			DuktapePushCharacterCore(pCtx, apCharCores[c]);
		else
			duk_push_null(pCtx);
		duk_put_prop_index(pCtx, ArrayCharCoresIdx, c);
	}

	duk_idx_t ArrayInputIdx = duk_push_array(pCtx);
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(apCharCores[c])
			DuktapePushNetObjPlayerInput(pCtx, &apCharCores[c]->m_Input);
		else
			duk_push_null(pCtx);
		duk_put_prop_index(pCtx, ArrayInputIdx, c);
	}

	m_Js.CallJsFunction(2);

	if(!m_Js.HasJsFunctionReturned()) {
		duk_pop(pCtx);
		return;
	}

	// EXPECTS RETURN: [array[CCharacterCore object], array[CNetObj_PlayerInput object]]
	if(duk_get_prop_index(pCtx, -1, 0))
	{
		for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(duk_get_prop_index(pCtx, -1, c))
			{
				if(!duk_is_null(pCtx, -1))
					DuktapeReadCharacterCore(pCtx, -1, apCharCores[c]);
				duk_pop(pCtx);
			}
		}
		duk_pop(pCtx);
	}
	if(duk_get_prop_index(pCtx, -1, 1))
	{
		for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(duk_get_prop_index(pCtx, -1, c))
			{
				if(!duk_is_null(pCtx, -1))
					DuktapeReadNetObjPlayerInput(pCtx, -1, &apCharCores[c]->m_Input);
				duk_pop(pCtx);
			}
		}
		duk_pop(pCtx);
	}

	duk_pop(pCtx);
}

void CDuckBridge::CharacterCorePostTick(CCharacterCore** apCharCores)
{
	if(!IsLoaded())
		return;

	duk_context* pCtx = m_Js.Ctx();

	if(!m_Js.GetJsFunction("OnCharacterCorePostTick")) {
		return;
	}

	// arguments (array[CCharacterCore object], array[CNetObj_PlayerInput object])
	duk_idx_t ArrayCharCoresIdx = duk_push_array(pCtx);
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(apCharCores[c])
			DuktapePushCharacterCore(pCtx, apCharCores[c]);
		else
			duk_push_null(pCtx);
		duk_put_prop_index(pCtx, ArrayCharCoresIdx, c);
	}

	duk_idx_t ArrayInputIdx = duk_push_array(pCtx);
	for(int c = 0; c < MAX_CLIENTS; c++)
	{
		if(apCharCores[c])
			DuktapePushNetObjPlayerInput(pCtx, &apCharCores[c]->m_Input);
		else
			duk_push_null(pCtx);
		duk_put_prop_index(pCtx, ArrayInputIdx, c);
	}

	m_Js.CallJsFunction(2);

	if(!m_Js.HasJsFunctionReturned()) {
		duk_pop(pCtx);
		return;
	}

	// EXPECTS RETURN: [array[CCharacterCore object], array[CNetObj_PlayerInput object]]
	if(duk_get_prop_index(pCtx, -1, 0))
	{
		for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(duk_get_prop_index(pCtx, -1, c))
			{
				if(!duk_is_null(pCtx, -1))
					DuktapeReadCharacterCore(pCtx, -1, apCharCores[c]);
				duk_pop(pCtx);
			}
		}
		duk_pop(pCtx);
	}
	if(duk_get_prop_index(pCtx, -1, 1))
	{
		for(int c = 0; c < MAX_CLIENTS; c++)
		{
			if(duk_get_prop_index(pCtx, -1, c))
			{
				if(!duk_is_null(pCtx, -1))
					DuktapeReadNetObjPlayerInput(pCtx, -1, &apCharCores[c]->m_Input);
				duk_pop(pCtx);
			}
		}
		duk_pop(pCtx);
	}

	duk_pop(pCtx);
}

void CDuckBridge::Predict(CWorldCore* pWorld)
{
	// TODO: fix this
	/*const int DiskCount = m_Collision.m_aDynamicDisks.size();
	for(int i = 0; i < DiskCount; i++)
	{
		m_Collision.m_aDynamicDisks[i].Tick(&m_Collision, pWorld);
	}
	for(int i = 0; i < DiskCount; i++)
	{
		m_Collision.m_aDynamicDisks[i].Move(&m_Collision, pWorld);
	}*/
}

void CDuckBridge::RenderPlayerWeapon(int WeaponID, vec2 Pos, float AttachAngle, float Angle, CTeeRenderInfo* pRenderInfo, float RecoilAlpha)
{
	const CWeaponCustom* pWeap = FindWeapon(WeaponID);
	if(!pWeap)
		return;

	if(!pWeap->TexWeaponHandle.IsValid())
		return;

	vec2 Dir = direction(Angle);

	Graphics()->TextureSet(pWeap->TexWeaponHandle);
	Graphics()->QuadsBegin();
	Graphics()->QuadsSetRotation(AttachAngle + Angle);

	if(Dir.x < 0)
		Graphics()->QuadsSetSubset(0, 1, 1, 0);

	vec2 p;
	p = Pos + Dir * pWeap->WeaponPos.x - Dir * RecoilAlpha * pWeap->Recoil;
	p.y += pWeap->WeaponPos.y;
	IGraphics::CQuadItem QuadItem(p.x, p.y, pWeap->WeaponSize.x, pWeap->WeaponSize.y);
	Graphics()->QuadsDraw(&QuadItem, 1);

	Graphics()->QuadsEnd();

	RenderTools()->RenderTeeHand(pRenderInfo, p, Dir, pWeap->HandAngle, pWeap->HandPos);
}

void CDuckBridge::RenderWeaponCursor(int WeaponID, vec2 Pos)
{
	const CWeaponCustom* pWeap = FindWeapon(WeaponID);
	if(!pWeap)
		return;

	if(!pWeap->TexCursorHandle.IsValid())
		return;

	Graphics()->TextureSet(pWeap->TexCursorHandle);
	Graphics()->QuadsBegin();

	// render cursor
	float CursorSize = 45.25483399593904156165;
	IGraphics::CQuadItem QuadItem(Pos.x, Pos.y, CursorSize, CursorSize);
	Graphics()->QuadsDraw(&QuadItem, 1);

	Graphics()->QuadsEnd();
}

void CDuckBridge::RenderWeaponAmmo(int WeaponID, vec2 Pos)
{
	// TODO: do ammo?
}

bool CDuckBridge::IsModAlreadyInstalled(const SHA256_DIGEST *pModSha256)
{
	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(*pModSha256, aSha256Str, sizeof(aSha256Str));

	char aModMainJsPath[512];
	str_copy(aModMainJsPath, "mods/", sizeof(aModMainJsPath));
	str_append(aModMainJsPath, aSha256Str, sizeof(aModMainJsPath));
	str_append(aModMainJsPath, "/main.js", sizeof(aModMainJsPath));

	IOHANDLE ModMainJs = Storage()->OpenFile(aModMainJsPath, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(ModMainJs)
	{
		io_close(ModMainJs);
		dbg_msg("duck", "mod is already installed on disk");
		return true;
	}
	return false;
}

bool CDuckBridge::ExtractAndInstallModZipBuffer(const CGrowBuffer *pHttpZipData, const SHA256_DIGEST *pModSha256)
{
	dbg_msg("unzip", "EXTRACTING AND INSTALLING MOD");

	char aUserModsPath[512];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "mods", aUserModsPath, sizeof(aUserModsPath));
	fs_makedir(aUserModsPath); // Teeworlds/mods (user storage)

	// TODO: reduce folder hash string length?
	SHA256_DIGEST Sha256 = sha256(pHttpZipData->m_pData, pHttpZipData->m_Size);
	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(Sha256, aSha256Str, sizeof(aSha256Str));

	if(Sha256 != *pModSha256)
	{
		dbg_msg("duck", "mod url sha256 and server sent mod sha256 mismatch, received sha256=%s", aSha256Str);
		// TODO: display error message
		return false;
	}

	char aModRootPath[512];
	str_copy(aModRootPath, aUserModsPath, sizeof(aModRootPath));
	str_append(aModRootPath, "/", sizeof(aModRootPath));
	str_append(aModRootPath, aSha256Str, sizeof(aModRootPath));


	// FIXME: remove this
	/*
	str_append(aUserModsPath, "/temp.zip", sizeof(aUserModsPath));
	IOHANDLE File = io_open(aUserMoodsPath, IOFLAG_WRITE);
	io_write(File, pHttpZipData->m_pData, pHttpZipData->m_Size);
	io_close(File);

	zip *pZipArchive = zip_open(aUserMoodsPath, 0, &Error);
	if(pZipArchive == NULL)
	{
		char aErrorBuff[512];
		zip_error_to_str(aErrorBuff, sizeof(aErrorBuff), Error, errno);
		dbg_msg("unzip", "Error opening '%s' [%s]", aUserMoodsPath, aErrorBuff);
		return false;
	}*/

	zip_error_t ZipError;
	zip_error_init(&ZipError);
	zip_source_t* pZipSrc = zip_source_buffer_create(pHttpZipData->m_pData, pHttpZipData->m_Size, 1, &ZipError);
	if(!pZipSrc)
	{
		dbg_msg("unzip", "Error creating zip source [%s]", zip_error_strerror(&ZipError));
		zip_error_fini(&ZipError);
		return false;
	}

	dbg_msg("unzip", "OPENING zip source %s", aSha256Str);

	// int Error = 0;
	zip *pZipArchive = zip_open_from_source(pZipSrc, 0, &ZipError);
	if(pZipArchive == NULL)
	{
		dbg_msg("unzip", "Error opening source [%s]", zip_error_strerror(&ZipError));
		zip_source_free(pZipSrc);
		zip_error_fini(&ZipError);
		return false;
	}
	zip_error_fini(&ZipError);

	dbg_msg("unzip", "CREATE directory '%s'", aModRootPath);
	if(fs_makedir(aModRootPath) != 0)
	{
		dbg_msg("unzip", "Failed to create directory '%s'", aModRootPath);
		return false;
	}

	const int EntryCount = zip_get_num_entries(pZipArchive, 0);

	// find required files
	const char* aRequiredFiles[] = {
		"main.js",
		"mod_info.json"
	};
	const int RequiredFilesCount = sizeof(aRequiredFiles)/sizeof(aRequiredFiles[0]);

	int FoundRequiredFilesCount = 0;
	for(int i = 0; i < EntryCount && FoundRequiredFilesCount < RequiredFilesCount; i++)
	{
		zip_stat_t EntryStat;
		if(zip_stat_index(pZipArchive, i, 0, &EntryStat) != 0)
			continue;

		const int NameLen = str_length(EntryStat.name);
		if(EntryStat.name[NameLen-1] != '/')
		{
			for(int r = 0; r < RequiredFilesCount; r++)
			{
				 // TODO: can 2 files have the same name?
				if(str_comp(EntryStat.name, aRequiredFiles[r]) == 0)
					FoundRequiredFilesCount++;
			}
		}
	}

	if(FoundRequiredFilesCount != RequiredFilesCount)
	{
		dbg_msg("duck", "mod is missing a required file, required files are: ");
		for(int r = 0; r < RequiredFilesCount; r++)
		{
			dbg_msg("duck", "    - %s", aRequiredFiles[r]);
		}
		return false;
	}

	// walk zip file tree and extract
	for(int i = 0; i < EntryCount; i++)
	{
		zip_stat_t EntryStat;
		if(zip_stat_index(pZipArchive, i, 0, &EntryStat) != 0)
			continue;

		// TODO: remove
		dbg_msg("unzip", "- name: %s, size: %llu, mtime: [%u]", EntryStat.name, EntryStat.size, (unsigned int)EntryStat.mtime);

		// TODO: sanitize folder name
		const int NameLen = str_length(EntryStat.name);
		if(EntryStat.name[NameLen-1] == '/')
		{
			// create sub directory
			char aSubFolder[512];
			str_copy(aSubFolder, aModRootPath, sizeof(aSubFolder));
			str_append(aSubFolder, "/", sizeof(aSubFolder));
			str_append(aSubFolder, EntryStat.name, sizeof(aSubFolder));

			dbg_msg("unzip", "CREATE SUB directory '%s'", aSubFolder);
			if(fs_makedir(aSubFolder) != 0)
			{
				dbg_msg("unzip", "Failed to create directory '%s'", aSubFolder);
				return false;
			}
		}
		else
		{
			// filter by extension
			if(!(str_endswith(EntryStat.name, ".js") || str_endswith(EntryStat.name, ".json") || str_endswith(EntryStat.name, ".png") || str_endswith(EntryStat.name, ".wv")))
				continue;

			// TODO: verify file type? Might be very expensive to do so.
			zip_file_t* pFileZip = zip_fopen_index(pZipArchive, i, 0);
			if(!pFileZip)
			{
				dbg_msg("unzip", "Error reading file '%s'", EntryStat.name);
				return false;
			}

			// create file on disk
			char aFilePath[256];
			str_copy(aFilePath, aModRootPath, sizeof(aFilePath));
			str_append(aFilePath, "/", sizeof(aFilePath));
			str_append(aFilePath, EntryStat.name, sizeof(aFilePath));

			IOHANDLE FileExtracted = io_open(aFilePath, IOFLAG_WRITE);
			if(!FileExtracted)
			{
				dbg_msg("unzip", "Error creating file '%s'", aFilePath);
				return false;
			}

			// read zip file data and write to file on disk
			char aReadBuff[1024];
			unsigned ReadCurrentSize = 0;
			while(ReadCurrentSize != EntryStat.size)
			{
				const int ReadLen = zip_fread(pFileZip, aReadBuff, sizeof(aReadBuff));
				if(ReadLen < 0)
				{
					dbg_msg("unzip", "Error reading file '%s'", EntryStat.name);
					return false;
				}
				io_write(FileExtracted, aReadBuff, ReadLen);
				ReadCurrentSize += ReadLen;
			}

			io_close(FileExtracted);
			zip_fclose(pFileZip);
		}
	}

	zip_source_close(pZipSrc);
	// NOTE: no need to call zip_source_free(pZipSrc), HttpBuffer::Release() already frees up the buffer

	//zip_close(pZipArchive);

#if 0
	unzFile ZipFile = unzOpen64(aPath);
	unz_global_info GlobalInfo;
	int r = unzGetGlobalInfo(ZipFile, &GlobalInfo);
	if(r != UNZ_OK)
	{
		dbg_msg("unzip", "could not read file global info (%d)", r);
		unzClose(ZipFile);
		dbg_break();
		return false;
	}

	for(int i = 0; i < GlobalInfo.number_entry; i++)
	{
		// Get info about current file.
		unz_file_info file_info;
		char filename[256];
		if(unzGetCurrentFileInfo(ZipFile, &file_info, filename, sizeof(filename), NULL, 0, NULL, 0) != UNZ_OK)
		{
			dbg_msg("unzip", "could not read file info");
			unzClose(ZipFile);
			return false;
		}

		dbg_msg("unzip", "FILE_ENTRY %s", filename);

		/*// Check if this entry is a directory or file.
		const size_t filename_length = str_length(filename);
		if(filename[ filename_length-1 ] == '/')
		{
			// Entry is a directory, so create it.
			printf("dir:%s\n", filename);
			//mkdir(filename);
		}
		else
		{
			// Entry is a file, so extract it.
			printf("file:%s\n", filename);
			if(unzOpenCurrentFile(ZipFile) != UNZ_OK)
			{
				dbg_msg("unzip", "could not open file");
				unzClose(ZipFile);
				return false;
			}

			// Open a file to write out the data.
			FILE *out = fopen(filename, "wb");
			if(out == NULL)
			{
				dbg_msg("unzip", "could not open destination file");
				unzCloseCurrentFile(ZipFile);
				unzClose(ZipFile);
				return false;
			}

			int error = UNZ_OK;
			do
			{
			error = unzReadCurrentFile(zipfile, read_buffer, READ_SIZE);
			if(error < 0)
			{
			printf("error %d\n", error);
			unzCloseCurrentFile(zipfile);
			unzClose(zipfile);
			return -1;
			}

			// Write data to file.
			if(error > 0)
			{
			fwrite(read_buffer, error, 1, out); // You should check return of fwrite...
			}
			} while (error > 0);

			fclose(out);
		}*/

		unzCloseCurrentFile(ZipFile);

		// Go the the next entry listed in the zip file.
		if((i+1) < GlobalInfo.number_entry)
		{
			if(unzGoToNextFile(ZipFile) != UNZ_OK)
			{
				dbg_msg("unzip", "cound not read next file");
				unzClose(ZipFile);
				return false;
			}
		}
	}

	unzClose(ZipFile);
#endif

	return true;
}

bool CDuckBridge::ExtractAndInstallModCompressedBuffer(const void *pCompBuff, int CompBuffSize, const SHA256_DIGEST *pModSha256)
{
	const bool IsConfigDebug = g_Config.m_Debug;

	if(IsConfigDebug)
		dbg_msg("unzip", "EXTRACTING AND INSTALLING *COMRPESSED* MOD");

	char aUserModsPath[512];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "mods", aUserModsPath, sizeof(aUserModsPath));
	fs_makedir(aUserModsPath); // Teeworlds/mods (user storage)

	// TODO: reduce folder hash string length?
	SHA256_DIGEST Sha256 = sha256(pCompBuff, CompBuffSize);
	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(Sha256, aSha256Str, sizeof(aSha256Str));

	if(Sha256 != *pModSha256)
	{
		dbg_msg("duck", "mod url sha256 and server sent mod sha256 mismatch, received sha256=%s", aSha256Str);
		// TODO: display error message
		return false;
	}

	// mod folder where we're going to extract the files
	char aModRootPath[512];
	str_copy(aModRootPath, aUserModsPath, sizeof(aModRootPath));
	str_append(aModRootPath, "/", sizeof(aModRootPath));
	str_append(aModRootPath, aSha256Str, sizeof(aModRootPath));

	// uncompress
	CGrowBuffer FilePackBuff;
	FilePackBuff.Grow(CompBuffSize * 3);

	uLongf DestSize = FilePackBuff.m_Capacity;
	int UncompRet = uncompress((Bytef*)FilePackBuff.m_pData, &DestSize, (const Bytef*)pCompBuff, CompBuffSize);
	FilePackBuff.m_Size = DestSize;

	int GrowAttempts = 4;
	while(UncompRet == Z_BUF_ERROR && GrowAttempts--)
	{
		FilePackBuff.Grow(FilePackBuff.m_Capacity * 2);
		DestSize = FilePackBuff.m_Capacity;
		UncompRet = uncompress((Bytef*)FilePackBuff.m_pData, &DestSize, (const Bytef*)pCompBuff, CompBuffSize);
		FilePackBuff.m_Size = DestSize;
	}

	if(UncompRet != Z_OK)
	{
		switch(UncompRet)
		{
			case Z_MEM_ERROR:
				dbg_msg("duck", "DecompressMod: Error, not enough memory");
				break;
			case Z_BUF_ERROR:
				dbg_msg("duck", "DecompressMod: Error, not enough room in the output buffer");
				break;
			case Z_DATA_ERROR:
				dbg_msg("duck", "DecompressMod: Error, data incomplete or corrupted");
				break;
			default:
				dbg_break(); // should never be reached
		}
		return false;
	}

	// read decompressed pack file

	// header
	const char* pFilePack = FilePackBuff.m_pData;
	const int FilePackSize = FilePackBuff.m_Size;
	const char* const pFilePackEnd = pFilePack + FilePackSize;

	if(str_comp_num(pFilePack, "DUCK", 4) != 0)
	{
		dbg_msg("duck", "DecompressMod: Error, invalid pack file");
		return false;
	}

	pFilePack += 4;

	// find required files
	const char* aRequiredFiles[] = {
		"main.js",
		"mod_info.json"
	};
	const int RequiredFilesCount = sizeof(aRequiredFiles)/sizeof(aRequiredFiles[0]);
	int FoundRequiredFilesCount = 0;

	// TODO: use packer
	const char* pCursor = pFilePack;
	while(pCursor < pFilePackEnd)
	{
		const int FilePathLen = *(int*)pCursor;
		pCursor += 4;
		const char* pFilePath = pCursor;
		pCursor += FilePathLen;
		const int FileSize = *(int*)pCursor;
		pCursor += 4;
		const char* pFileData = pCursor;
		pCursor += FileSize;

		char aFilePath[512];
		dbg_assert(FilePathLen < sizeof(aFilePath)-1, "FilePathLen too large");
		mem_move(aFilePath, pFilePath, FilePathLen);
		aFilePath[FilePathLen] = 0;

		if(IsConfigDebug)
			dbg_msg("duck", "file path=%s size=%d", aFilePath, FileSize);

		for(int r = 0; r < RequiredFilesCount; r++)
		{
			// TODO: can 2 files have the same name?
			// TODO: not very efficient, but oh well
			const char* pFind = str_find(aFilePath, aRequiredFiles[r]);
			if(pFind && FilePathLen-(pFind-aFilePath) == str_length(aRequiredFiles[r]))
				FoundRequiredFilesCount++;
		}
	}

	if(FoundRequiredFilesCount != RequiredFilesCount)
	{
		dbg_msg("duck", "mod is missing a required file, required files are: ");
		for(int r = 0; r < RequiredFilesCount; r++)
		{
			dbg_msg("duck", "    - %s", aRequiredFiles[r]);
		}
		return false;
	}

	if(IsConfigDebug)
		dbg_msg("duck", "CREATE directory '%s'", aModRootPath);
	if(fs_makedir(aModRootPath) != 0)
	{
		dbg_msg("duck", "DecompressMod: Error, failed to create directory '%s'", aModRootPath);
		return false;
	}

	pCursor = pFilePack;
	while(pCursor < pFilePackEnd)
	{
		const int FilePathLen = *(int*)pCursor;
		pCursor += 4;
		const char* pFilePath = pCursor;
		pCursor += FilePathLen;
		const int FileSize = *(int*)pCursor;
		pCursor += 4;
		const char* pFileData = pCursor;
		pCursor += FileSize;

		char aFilePath[512];
		dbg_assert(FilePathLen < sizeof(aFilePath)-1, "FilePathLen too large");
		mem_move(aFilePath, pFilePath, FilePathLen);
		aFilePath[FilePathLen] = 0;

		// create directory to hold the file (if needed)
		const char* pSlashFound = aFilePath;
		do
		{
			pSlashFound = str_find(pSlashFound, "/");
			if(pSlashFound)
			{
				char aDir[512];
				str_format(aDir, sizeof(aDir), "%.*s", pSlashFound-aFilePath, aFilePath);

				if(IsConfigDebug)
					dbg_msg("duck", "CREATE SUBDIR dir=%s", aDir);

				char aDirPath[512];
				str_copy(aDirPath, aModRootPath, sizeof(aDirPath));
				str_append(aDirPath, "/", sizeof(aDirPath));
				str_append(aDirPath, aDir, sizeof(aDirPath));

				if(fs_makedir(aDirPath) != 0)
				{
					dbg_msg("duck", "DecompressMod: Error, failed to create directory '%s'", aDirPath);
					return false;
				}

				pSlashFound++;
			}
		}
		while(pSlashFound);

		// create file on disk
		char aDiskFilePath[256];
		str_copy(aDiskFilePath, aModRootPath, sizeof(aDiskFilePath));
		str_append(aDiskFilePath, "/", sizeof(aDiskFilePath));
		str_append(aDiskFilePath, aFilePath, sizeof(aDiskFilePath));

		IOHANDLE FileExtracted = io_open(aDiskFilePath, IOFLAG_WRITE);
		if(!FileExtracted)
		{
			dbg_msg("duck", "Error creating file '%s'", aDiskFilePath);
			return false;
		}

		io_write(FileExtracted, pFileData, FileSize);
		io_close(FileExtracted);
	}

	return true;
}

struct CPath
{
	char m_aBuff[512];
};

struct CFileSearch
{
	CPath m_BaseBath;
	array<CPath>* m_paFilePathList;
};

static int ListDirCallback(const char* pName, int IsDir, int Type, void *pUser)
{
	CFileSearch FileSearch = *(CFileSearch*)pUser; // copy

	if(IsDir)
	{
		if(pName[0] != '.')
		{
			//dbg_msg("duck", "ListDirCallback dir='%s'", pName);
			str_append(FileSearch.m_BaseBath.m_aBuff, "/", sizeof(FileSearch.m_BaseBath.m_aBuff));
			str_append(FileSearch.m_BaseBath.m_aBuff, pName, sizeof(FileSearch.m_BaseBath.m_aBuff));
			//dbg_msg("duck", "recursing... dir='%s'", DirPath.m_aBuff);
			fs_listdir(FileSearch.m_BaseBath.m_aBuff, ListDirCallback, Type + 1, &FileSearch);
		}
	}
	else
	{
		CPath FileStr;
		str_copy(FileStr.m_aBuff, pName, sizeof(FileStr.m_aBuff));

		CPath FilePath = FileSearch.m_BaseBath;
		str_append(FilePath.m_aBuff, "/", sizeof(FilePath.m_aBuff));
		str_append(FilePath.m_aBuff, pName, sizeof(FilePath.m_aBuff));
		FileSearch.m_paFilePathList->add(FilePath);
		//dbg_msg("duck", "ListDirCallback file='%s'", pName);
	}

	return 0;
}

bool CDuckBridge::LoadModFilesFromDisk(const SHA256_DIGEST *pModSha256)
{
	char aModRootPath[512];
	char aSha256Str[SHA256_MAXSTRSIZE];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, "mods", aModRootPath, sizeof(aModRootPath));
	const int SaveDirPathLen = str_length(aModRootPath)-4;
	sha256_str(*pModSha256, aSha256Str, sizeof(aSha256Str));
	str_append(aModRootPath, "/", sizeof(aModRootPath));
	str_append(aModRootPath, aSha256Str, sizeof(aModRootPath));
	const int ModRootDirLen = str_length(aModRootPath);

	// get files recursively on disk
	array<CPath> aFilePathList;
	CFileSearch FileSearch;
	str_copy(FileSearch.m_BaseBath.m_aBuff, aModRootPath, sizeof(FileSearch.m_BaseBath.m_aBuff));
	FileSearch.m_paFilePathList = &aFilePathList;

	fs_listdir(aModRootPath, ListDirCallback, 1, &FileSearch);

	// reset mod
	OnModReset();

	const int FileCount = aFilePathList.size();
	const CPath* pFilePaths = aFilePathList.base_ptr();
	for(int i = 0; i < FileCount; i++)
	{
		//dbg_msg("duck", "file='%s'", pFilePaths[i].m_aBuff);
		if(str_endswith(pFilePaths[i].m_aBuff, ".js"))
		{
			const char* pRelPath = pFilePaths[i].m_aBuff+ModRootDirLen+1;
			bool Loaded = m_Js.LoadJsScriptFile(pFilePaths[i].m_aBuff, pRelPath);
			dbg_assert(Loaded, "error loading js script");
			// TODO: show error instead of breaking
		}
		else if(str_endswith(pFilePaths[i].m_aBuff, ".png"))
		{
			const char* pTextureName = pFilePaths[i].m_aBuff+ModRootDirLen+1;
			const char* pTextureRelPath = pFilePaths[i].m_aBuff+SaveDirPathLen;
			const bool Loaded = LoadTexture(pTextureRelPath, pTextureName);
			dbg_assert(Loaded, "error loading png image");
			dbg_msg("duck", "image loaded '%s' (%x)", pTextureName, m_aTextures[m_aTextures.size()-1].m_Hash);
			// TODO: show error instead of breaking

			if(str_startswith(pTextureName, "skins/")) {
				pTextureName += 6;
				const char* pPartEnd = str_find(pTextureName, "/");
				if(!str_find(pPartEnd+1, "/")) {
					dbg_msg("duck", "skin part name = '%.*s'", pPartEnd-pTextureName, pTextureName);
					char aPart[256];
					str_format(aPart, sizeof(aPart), "%.*s", pPartEnd-pTextureName, pTextureName);
					AddSkinPart(aPart, pPartEnd+1, m_aTextures[m_aTextures.size()-1].m_Handle);
				}
			}
		}
	}

	m_IsModLoaded = true;
	m_Js.OnModLoaded();
	return true;
}

bool CDuckBridge::StartDuckModHttpDownload(const char *pModUrl, const SHA256_DIGEST *pModSha256)
{
	dbg_assert(!IsModAlreadyInstalled(pModSha256), "mod is already installed, check it before calling this");

	CGrowBuffer Buff;
	HttpRequestPage(pModUrl, &Buff);

	bool IsUnzipped = ExtractAndInstallModZipBuffer(&Buff, pModSha256);
	dbg_assert(IsUnzipped, "Unzipped to disk: rip in peace");

	Buff.Release();

	if(!IsUnzipped)
		return false;

	bool IsLoaded = LoadModFilesFromDisk(pModSha256);
	dbg_assert(IsLoaded, "Loaded from disk: rip in peace");

	dbg_msg("duck", "mod loaded url='%s'", pModUrl);
	return IsLoaded;
}

bool CDuckBridge::TryLoadInstalledDuckMod(const SHA256_DIGEST *pModSha256)
{
	if(!IsModAlreadyInstalled(pModSha256))
		return false;

	bool IsLoaded = LoadModFilesFromDisk(pModSha256);
	dbg_assert(IsLoaded, "Loaded from disk: rip in peace");

	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(*pModSha256, aSha256Str, sizeof(aSha256Str));
	dbg_msg("duck", "mod loaded (already installed) sha256='%s'", aSha256Str);
	return IsLoaded;
}

bool CDuckBridge::InstallAndLoadDuckModFromZipBuffer(const void *pBuffer, int BufferSize, const SHA256_DIGEST *pModSha256)
{
	dbg_assert(!IsModAlreadyInstalled(pModSha256), "mod is already installed, check it before calling this");

	bool IsUnzipped = ExtractAndInstallModCompressedBuffer(pBuffer, BufferSize, pModSha256);
	dbg_assert(IsUnzipped, "Unzipped to disk: rip in peace");

	if(!IsUnzipped)
		return false;

	bool IsLoaded = LoadModFilesFromDisk(pModSha256);
	dbg_assert(IsLoaded, "Loaded from disk: rip in peace");

	char aSha256Str[SHA256_MAXSTRSIZE];
	sha256_str(*pModSha256, aSha256Str, sizeof(aSha256Str));
	dbg_msg("duck", "mod loaded from zip buffer sha256='%s'", aSha256Str);
	return IsLoaded;
}

void CDuckBridge::OnInit()
{
	m_Js.m_pBridge = this;
	m_CurrentDrawSpace = 0;
	m_CurrentPacketFlags = -1;
}

void CDuckBridge::OnShutdown()
{
	m_Js.Shutdown();
}

void CDuckBridge::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE || !IsLoaded())
		return;

	m_FrameAllocator.Clear(); // clear frame allocator

	duk_context* pCtx = m_Js.Ctx();

	const float LocalTime = Client()->LocalTime();
	const float IntraGameTick = Client()->IntraGameTick();

	// Call OnRender(LocalTime, IntraGameTick)
	if(m_Js.GetJsFunction("OnRender"))
	{
		duk_push_number(pCtx, LocalTime);
		duk_push_number(pCtx, IntraGameTick);

		m_Js.CallJsFunction(2);

		duk_pop(pCtx);
	}

	static float LastTime = LocalTime;
	static float Accumulator = 0.0f;
	const float UPDATE_RATE = 1.0/60.0;

	Accumulator += LocalTime - LastTime;
	LastTime = LocalTime;

	int UpdateCount = 0;
	while(Accumulator > UPDATE_RATE)
	{
		// Call OnUpdate(LocalTime, IntraGameTick)
		if(m_Js.GetJsFunction("OnUpdate"))
		{
			duk_push_number(pCtx, LocalTime);
			duk_push_number(pCtx, IntraGameTick);

			m_Js.CallJsFunction(2);

			duk_pop(pCtx);
		}

		Accumulator -= UPDATE_RATE;
		UpdateCount++;

		if(UpdateCount > 2) {
			Accumulator = 0.0;
			break;
		}
	}

	// detect stack leak
	dbg_assert(duk_get_top(m_Js.Ctx()) == 0, "stack leak");
}

void CDuckBridge::OnMessage(int Msg, void *pRawMsg)
{
	if(!IsLoaded()) {
		return;
	}

	m_Js.OnMessage(Msg, pRawMsg);
}

void CDuckBridge::OnStateChange(int NewState, int OldState)
{
	if(OldState != IClient::STATE_OFFLINE && NewState == IClient::STATE_OFFLINE)
	{
		OnModReset();
	}
}

bool CDuckBridge::OnInput(IInput::CEvent e)
{
	if(!IsLoaded()) {
		return false;
	}

	m_Js.OnInput(e);
	return false;
}

void CDuckBridge::OnModReset()
{
	m_Js.ResetDukContext();
	Reset();
	m_IsModLoaded = false;
}

void CDuckBridge::OnModUnload()
{
	if(m_Js.m_pDukContext)
		duk_destroy_heap(m_Js.m_pDukContext);
	m_Js.m_pDukContext = 0;

	Reset();
	m_IsModLoaded = false;
}
