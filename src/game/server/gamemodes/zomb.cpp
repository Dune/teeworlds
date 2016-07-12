// LordSk
#include "zomb.h"
#include <stdio.h>
#include <io.h>
#include <engine/server.h>
#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <game/server/entity.h>
#include <game/server/player.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>

/* TODO:
 * - Queen, Dominant
 * - Boss zombies?
 * - tweak bull
 */

static char msgBuff__[256];
#define dbg_zomb_msg(...)\
	memset(msgBuff__, 0, sizeof(msgBuff__));\
	str_format(msgBuff__, 256, ##__VA_ARGS__);\
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "zomb", msgBuff__);

#define std_zomb_msg(...)\
	memset(msgBuff__, 0, sizeof(msgBuff__));\
	str_format(msgBuff__, 256, ##__VA_ARGS__);\
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zomb", msgBuff__);

#define ZombCID(id) (id + MAX_SURVIVORS)
#define NO_TARGET -1
#define SecondsToTick(sec) (i32)(sec * SERVER_TICK_SPEED)

#define HOOK_RANGE 375.f
#define DEFAULT_ENRAGE_TIME SecondsToTick(30)

#define BOOMER_EXPLOSION_INNER_RADIUS 150.f
#define BOOMER_EXPLOSION_OUTER_RADIUS 300.f

#define BULL_CHARGE_CD (SecondsToTick(3))
#define BULL_CHARGE_SPEED 1850.f
#define BULL_KNOCKBACK_FORCE 30.f

#define MUDGE_PULL_STR 1.3f

#define HUNTER_CHARGE_CD (SecondsToTick(1.5f))
#define HUNTER_CHARGE_SPEED 2700.f
#define HUNTER_CHARGE_DMG 3

#define SPAWN_INTERVAL (SecondsToTick(0.4f))
#define WAVE_WAIT_TIME (SecondsToTick(10))

enum {
	SKINPART_BODY = 0,
	SKINPART_MARKING,
	SKINPART_DECORATION,
	SKINPART_HANDS,
	SKINPART_FEET,
	SKINPART_EYES
};

enum {
	ZTYPE_BASIC = 0,
	ZTYPE_TANK,
	ZTYPE_BOOMER,
	ZTYPE_BULL,
	ZTYPE_MUDGE,
	ZTYPE_HUNTER,
	ZTYPE_MAX,
	ZTYPE_INVALID,
};

static const char* g_ZombName[] = {
	"zombie",
	"Tank",
	"Boomer",
	"Bull",
	"Mudge",
	"Hunter"
};

static const u32 g_ZombMaxHealth[] = {
	7, // ZTYPE_BASIC
	35, // ZTYPE_TANK
	12, // ZTYPE_BOOMER
	20, // ZTYPE_BULL
	8, // ZTYPE_MUDGE
	7 // ZTYPE_HUNTER
};

static const f32 g_ZombAttackSpeed[] = {
	1.5f, // ZTYPE_BASIC
	1.f, // ZTYPE_TANK
	0.0001f, // ZTYPE_BOOMER
	1.0f, // ZTYPE_BULL
	3.0f, // ZTYPE_MUDGE
	5.0f // ZTYPE_HUNTER
};

static const i32 g_ZombAttackDmg[] = {
	1, // ZTYPE_BASIC
	6, // ZTYPE_TANK
	10, // ZTYPE_BOOMER
	4, // ZTYPE_BULL
	1, // ZTYPE_MUDGE
	1 // ZTYPE_HUNTER
};

static const f32 g_ZombKnockbackMultiplier[] = {
	2.f, // ZTYPE_BASIC
	0.f, // ZTYPE_TANK
	0.f, // ZTYPE_BOOMER
	0.5f, // ZTYPE_BULL
	0.f, // ZTYPE_MUDGE
	1.f, // ZTYPE_HUNTER
};

static const i32 g_ZombGrabLimit[] = {
	SecondsToTick(0.2f), // ZTYPE_BASIC
	SecondsToTick(2.5f), // ZTYPE_TANK
	SecondsToTick(1.f), // ZTYPE_BOOMER
	SecondsToTick(1.f), // ZTYPE_BULL
	SecondsToTick(30.f), // ZTYPE_MUDGE
	SecondsToTick(0.5f) // ZTYPE_HUNTER
};

static const i32 g_ZombHookCD[] = {
	SecondsToTick(2.f), // ZTYPE_BASIC
	SecondsToTick(2.f), // ZTYPE_TANK
	SecondsToTick(0.5f), // ZTYPE_BOOMER
	SecondsToTick(3.f), // ZTYPE_BULL
	SecondsToTick(3.f), // ZTYPE_MUDGE
	SecondsToTick(2.f) // ZTYPE_HUNTER
};

enum {
	BUFF_ENRAGED = 1,
	BUFF_HEALING = (1 << 1),
	BUFF_ARMORED = (1 << 2),
	BUFF_ELITE = (1 << 3)
};

enum {
	ZOMBIE_GROUNDJUMP = 1,
	ZOMBIE_AIRJUMP,
	ZOMBIE_BIGJUMP
};

enum {
	ZSTATE_NONE = 0,
	ZSTATE_WAVE_GAME,
	ZSTATE_SURV_GAME,
};

static i32 RandInt(i32 min, i32 max)
{
	dbg_assert(max > min, "RandInt(min, max) max must be > min");
	return (min + (rand() / (f32)RAND_MAX) * (max + 1 - min));
}

void CGameControllerZOMB::SpawnZombie(i32 zid, u8 type, bool isElite, u32 enrageTime)
{
	bool isEnraged = (enrageTime == 0);
	// do we need to update clients
	bool needSendInfos = false;
	if(m_ZombType[zid] != type ||
	   (m_ZombBuff[zid]&BUFF_ELITE) != isElite ||
	   (m_ZombBuff[zid]&BUFF_ENRAGED) != isEnraged) {
		needSendInfos = true;
	}

	m_ZombAlive[zid] = true;
	m_ZombType[zid] = type;
	m_ZombHealth[zid] = g_ZombMaxHealth[type];
	m_ZombBuff[zid] = 0;
	m_ZombActiveWeapon[zid] = WEAPON_HAMMER;

	if(isElite) {
		m_ZombHealth[zid] *= 2;
		m_ZombBuff[zid] |= BUFF_ELITE;
	}

	if(isEnraged) {
		m_ZombBuff[zid] |= BUFF_ENRAGED;
	}

	vec2 spawnPos = m_ZombSpawnPoint[(zid + m_Seed)%m_ZombSpawnPointCount];
	m_ZombCharCore[zid].Reset();
	m_ZombCharCore[zid].m_Pos = spawnPos;
	GameServer()->m_World.m_Core.m_apCharacters[ZombCID(zid)] = &m_ZombCharCore[zid];

	m_ZombInput[zid] = CNetObj_PlayerInput();
	m_ZombSurvTarget[zid] = NO_TARGET;
	m_ZombJumpClock[zid] = 0;
	m_ZombAttackClock[zid] = SecondsToTick(1.f / g_ZombAttackSpeed[type]);
	m_ZombEnrageClock[zid] = SecondsToTick(enrageTime);
	m_ZombExplodeClock[zid] = -1;
	m_ZombHookGrabClock[zid] = 0;
	m_ZombChargeClock[zid] = 0;
	m_ZombChargingClock[zid] = -1;

	if(needSendInfos) {
		for(u32 i = 0; i < MAX_SURVIVORS; ++i) {
			if(GameServer()->m_apPlayers[i] && Server()->ClientIngame(i)) {
				SendZombieInfos(zid, i);
			}
		}
	}

	GameServer()->CreatePlayerSpawn(spawnPos);
}

void CGameControllerZOMB::KillZombie(i32 zid, i32 killerCID)
{
	m_ZombAlive[zid] = false;
	GameServer()->m_World.m_Core.m_apCharacters[ZombCID(zid)] = 0;
	GameServer()->CreateSound(m_ZombCharCore[zid].m_Pos, SOUND_PLAYER_DIE);
	GameServer()->CreateSound(m_ZombCharCore[zid].m_Pos, SOUND_PLAYER_PAIN_LONG);
	GameServer()->CreateDeath(m_ZombCharCore[zid].m_Pos, ZombCID(zid));
	// TODO: send a kill message?

	if(m_RedFlagCarrier == ZombCID(zid)) {
		m_RedFlagCarrier = -1;
	}
}

void CGameControllerZOMB::SwingHammer(i32 zid, u32 dmg, f32 knockback)
{
	const vec2& zpos = m_ZombCharCore[zid].m_Pos;
	GameServer()->CreateSound(zpos, SOUND_HAMMER_FIRE);

	vec2 hitDir = normalize(vec2(m_ZombInput[zid].m_TargetX, m_ZombInput[zid].m_TargetY));
	vec2 hitPos = zpos + hitDir * 21.f; // NOTE: different reach per zombie type?

	CCharacter *apEnts[MAX_CLIENTS];

	// NOTE: same here for radius
	i32 count = GameServer()->m_World.FindEntities(hitPos, 14.f, (CEntity**)apEnts,
					MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

	for(i32 i = 0; i < count; ++i) {
		CCharacter *pTarget = apEnts[i];
		const vec2& targetPos = pTarget->GetPos();

		if(GameServer()->Collision()->IntersectLine(hitPos, targetPos, NULL, NULL)) {
			continue;
		}

		// set his velocity to fast upward (for now)
		if(length(targetPos-hitPos) > 0.0f) {
			GameServer()->CreateHammerHit(targetPos-normalize(targetPos-hitPos)*14.f);
		}
		else {
			GameServer()->CreateHammerHit(hitPos);
		}

		vec2 kbDir;
		if(length(targetPos - zpos) > 0.0f) {
			kbDir = normalize(targetPos - zpos);
		}
		else {
			kbDir = vec2(0.f, -1.f);
		}

		vec2 kbForce = vec2(0.f, -1.f) + normalize(kbDir + vec2(0.f, -1.1f)) * knockback;

		pTarget->TakeDamage(kbForce, dmg, ZombCID(zid), WEAPON_HAMMER);
	}

	m_ZombAttackTick[zid] = m_Tick;
}

struct Node
{
	ivec2 pos;
	f32 g, f;
	Node* pParent;

	Node() = default;
	Node(ivec2 pos_, f32 g_, f32 h_, Node* pParent_):
		pos(pos_),
		g(g_),
		f(g_ + h_),
		pParent(pParent_)
	{}
};

inline Node* addToList_(Node** pNodeList, u32* pCount, Node* pNode)
{
	dbg_assert((*pCount)+1 < MAX_MAP_SIZE, "list is full");\
	pNodeList[(*pCount)++] = pNode;
	return pNodeList[(*pCount)-1];
}

#define addToList(list, node) addToList_(list, &list##Count, (node))

inline Node* addNode_(Node* pNodeList, u32* pCount, Node node)
{
	dbg_assert((*pCount)+1 < MAX_MAP_SIZE, "list is full");\
	pNodeList[(*pCount)++] = node;
	return &pNodeList[(*pCount)-1];
}

#define addNode(node) addNode_(nodeList, &nodeListCount, (node))

inline Node* searchList_(Node* pNodeList, u32 count, ivec2 pos)
{
	for(u32 i = 0; i < count; ++i) {
		if(pNodeList[i].pos == pos) {
			return &pNodeList[i];
		}
	}
	return 0;
}

#define searchList(list, pos) searchList_(list, list##Count, pos)

static i32 compareNodePtr(const void* a, const void* b)
{
	const Node* na = *(const Node**)a;
	const Node* nb = *(const Node**)b;
	if(na->f < nb->f) return -1;
	if(na->f > nb->f) return 1;
	return 0;
}

inline bool CGameControllerZOMB::InMapBounds(const ivec2& pos)
{
	return (pos.x >= 0 && pos.x < (i32)m_MapWidth &&
			pos.y >= 0 && pos.y < (i32)m_MapHeight);
}

inline bool CGameControllerZOMB::IsBlocked(const ivec2& pos)
{
	return (!InMapBounds(pos) || m_Map[pos.y * m_MapWidth + pos.x] != 0);
}

static Node* openList[MAX_MAP_SIZE];
static Node* closedList[MAX_MAP_SIZE];
static u32 openListCount = 0;
static u32 closedListCount = 0;
static Node nodeList[MAX_MAP_SIZE];
static u32 nodeListCount = 0;

bool CGameControllerZOMB::JumpStraight(const ivec2& start, const ivec2& dir, const ivec2& goal,
									   i32* out_pJumps)
{
	ivec2 jumpPos = start;
	ivec2 forcedCheckDir[2]; // places to check for walls

	if(dir.x != 0) {
		forcedCheckDir[0] = ivec2(0, 1);
		forcedCheckDir[1] = ivec2(0, -1);
	}
	else {
		forcedCheckDir[0] = ivec2(1, 0);
		forcedCheckDir[1] = ivec2(-1, 0);
	}

	*out_pJumps = 0;
	while(1) {
		if(jumpPos == goal) {
			return true;
		}

		// hit a wall, diregard jump
		if(!InMapBounds(jumpPos) || IsBlocked(jumpPos)) {
			return false;
		}

		// forced neighours check
		ivec2 wallPos = jumpPos + forcedCheckDir[0];
		ivec2 freePos = jumpPos + forcedCheckDir[0] + dir;
		if(IsBlocked(wallPos) && !IsBlocked(freePos)) {
			return true;
		}

		wallPos = jumpPos + forcedCheckDir[1];
		freePos = jumpPos + forcedCheckDir[1] + dir;
		if(IsBlocked(wallPos) && !IsBlocked(freePos)) {
			return true;
		}

		jumpPos += dir;
		++(*out_pJumps);
	}

	return false;
}

bool CGameControllerZOMB::JumpDiagonal(const ivec2& start, const ivec2& dir, const ivec2& goal,
									   i32* out_pJumps)
{
	ivec2 jumpPos = start;
	ivec2 forcedCheckDir[2]; // places to check for walls
	forcedCheckDir[0] = ivec2(-dir.x, 0);
	forcedCheckDir[1] = ivec2(0, -dir.y);

	ivec2 straigthDirs[] = {
		ivec2(dir.x, 0),
		ivec2(0, dir.y)
	};

	*out_pJumps = 0;
	while(1) {
		if(jumpPos == goal) {
			return true;
		}

		// check veritcal/horizontal
		for(u32 i = 0; i < 2; ++i) {
			i32 j;
			if(JumpStraight(jumpPos, straigthDirs[i], goal, &j)) {
				return true;
			}
		}

		// hit a wall, diregard jump
		if(!InMapBounds(jumpPos) || IsBlocked(jumpPos)) {
			return false;
		}

		// forced neighours check
		ivec2 wallPos = jumpPos + forcedCheckDir[0];
		ivec2 freePos = jumpPos + forcedCheckDir[0] + ivec2(0, dir.y);
		if(IsBlocked(wallPos) && !IsBlocked(freePos)) {
			return true;
		}

		wallPos = jumpPos + forcedCheckDir[1];
		freePos = jumpPos + forcedCheckDir[1] + ivec2(dir.x, 0);
		if(IsBlocked(wallPos) && !IsBlocked(freePos)) {
			return true;
		}

		jumpPos += dir;
		++(*out_pJumps);
	}

	return false;
}

vec2 CGameControllerZOMB::PathFind(vec2 start, vec2 end)
{
	ivec2 mStart(start.x / 32, start.y / 32);
	ivec2 mEnd(end.x / 32, end.y / 32);

	if(mStart == mEnd) {
		return end;
	}

	if(IsBlocked(mStart) || IsBlocked(mStart)) {
		dbg_zomb_msg("Error: PathFind() start and end point must be clear.");
		return end;
	}

	bool pathFound = false;
	openListCount = 0;
	closedListCount = 0;
	nodeListCount = 0;

#ifdef CONF_DEBUG
	m_DbgPathLen = 0;
	m_DbgLinesCount = 0;
	u32 maxIterations = g_Config.m_DbgPathFindIterations;
#endif

	Node* pFirst = addNode(Node(mStart, 0, 0, nullptr));
	addToList(openList, pFirst);

	u32 iterations = 0;
	while(openListCount > 0 && !pathFound) {
		qsort(openList, openListCount, sizeof(Node*), compareNodePtr);

		// pop first node from open list and add it to closed list
		Node* pCurrent = openList[0];
		openList[0] = openList[openListCount-1];
		--openListCount;
		addToList(closedList, pCurrent);

		// neighbours
		ivec2 nbDir[8];
		u32 nbCount = 0;
		const ivec2& cp = pCurrent->pos;

		// first node (TODO: move this out of the loop)
		if(!pCurrent->pParent) {
			for(i32 x = -1; x < 2; ++x) {
				for(i32 y = -1; y < 2; ++y) {
					ivec2 dir(x, y);
					ivec2 succPos = cp + dir;
					if((x == 0 && y == 0) ||
					   !InMapBounds(succPos) ||
					   IsBlocked(succPos)) {
						continue;
					}
					nbDir[nbCount++] = dir;
				}
			}
		}
		else {
			ivec2 fromDir(clamp(cp.x - pCurrent->pParent->pos.x, -1, 1),
						  clamp(cp.y - pCurrent->pParent->pos.y, -1, 1));



			// straight (add fromDir + adjacent diags)
			if(fromDir.x == 0 || fromDir.y == 0) {
				if(!IsBlocked(cp + fromDir)) {
					nbDir[nbCount++] = fromDir;

					if(fromDir.x == 0) {
						ivec2 diagR(1, fromDir.y);
						if(!IsBlocked(cp + diagR)) {
							nbDir[nbCount++] = diagR;
						}
						ivec2 diagL(-1, fromDir.y);
						if(!IsBlocked(cp + diagL)) {
							nbDir[nbCount++] = diagL;
						}
					}
					else if(fromDir.y == 0) {
						ivec2 diagB(fromDir.x, 1);
						if(!IsBlocked(cp + diagB)) {
							nbDir[nbCount++] = diagB;
						}
						ivec2 diagT(fromDir.x, -1);
						if(!IsBlocked(cp + diagT)) {
							nbDir[nbCount++] = diagT;
						}
					}
				}
			}
			// diagonal (add diag + adjacent straight dirs + forced neighbours)
			else {
				ivec2 stX(fromDir.x, 0);
				ivec2 stY(0, fromDir.y);

				if(!IsBlocked(cp + fromDir) &&
				   (!IsBlocked(cp + stX) || !IsBlocked(cp + stY))) {
					nbDir[nbCount++] = fromDir;
				}
				if(!IsBlocked(cp + stX)) {
					nbDir[nbCount++] = stX;
				}
				if(!IsBlocked(cp + stY)) {
					nbDir[nbCount++] = stY;
				}

				// forced neighbours
				ivec2 fn1(-stX.x, stY.y);
				if(IsBlocked(cp - stX) && !IsBlocked(cp + stY) && !IsBlocked(cp + fn1)) {
					nbDir[nbCount++] = fn1;
				}
				ivec2 fn2(stX.x, -stY.y);
				if(IsBlocked(cp - stY) && !IsBlocked(cp + stX) && !IsBlocked(cp + fn2)) {
					nbDir[nbCount++] = fn2;
				}
			}
		}

		// jump
		for(u32 n = 0; n < nbCount; ++n) {
			const ivec2& succDir = nbDir[n];
			ivec2 succPos = pCurrent->pos + succDir;

			if(succDir.x == 0 || succDir.y == 0) {
				i32 jumps;
				if(JumpStraight(succPos, succDir, mEnd, &jumps)) {
					ivec2 jumpPos = succPos + succDir * jumps;
					f32 g = pCurrent->g + jumps;
					f32 h = abs(mEnd.x - jumpPos.x) + abs(mEnd.y - jumpPos.y);

					if(jumpPos == mEnd) {
						addToList(closedList, addNode(Node(jumpPos, 0, 0, pCurrent)));
						pathFound = true;
						break;
					}
					else {
						Node* pSearchNode = searchList(nodeList, jumpPos);
						if(pSearchNode) {
							f32 f = g + h;
							if(f < pSearchNode->f) {
								pSearchNode->f = f;
								pSearchNode->g = g;
								pSearchNode->pParent = pCurrent;
							}
						}
						else {
							Node* pJumpNode = addNode(Node(jumpPos, g, h, pCurrent));
							addToList(openList, pJumpNode);
						}
					}
				}
			}
			else {
				i32 jumps = 0;
				if(JumpDiagonal(succPos, succDir, mEnd, &jumps)) {
					ivec2 jumpPos = succPos + succDir * jumps;
					f32 g = pCurrent->g + jumps * 2.f;
					f32 h = abs(mEnd.x - jumpPos.x) + abs(mEnd.y - jumpPos.y);

					if(jumpPos == mEnd) {
						addToList(closedList, addNode(Node(jumpPos, 0, 0, pCurrent)));
						pathFound = true;
						break;
					}
					else {
						Node* pSearchNode = searchList(nodeList, jumpPos);
						if(pSearchNode) {
							f32 f = g + h;
							if(f < pSearchNode->f) {
								pSearchNode->f = f;
								pSearchNode->g = g;
								pSearchNode->pParent = pCurrent;
							}
						}
						else {
							Node* pJumpNode = addNode(Node(jumpPos, g, h, pCurrent));
							addToList(openList, pJumpNode);
						}
					}
				}
			}
		}

		// TODO: remove this safeguard
		++iterations;
#ifdef CONF_DEBUG
		if(iterations > maxIterations) {
			break;
		}
#endif
	}

	Node* pCur = closedList[closedListCount-1];
	ivec2 next = pCur->pos;
	while(pCur && pCur->pParent) {
		next = pCur->pos;
		pCur = pCur->pParent;
	}

	return vec2(next.x * 32.f + 16.f, next.y * 32.f + 16.f);
}

inline i32 PackColor(i32 hue, i32 sat, i32 lgt, i32 alpha = 255)
{
	return ((alpha << 24) + (hue << 16) + (sat << 8) + lgt);
}

void CGameControllerZOMB::SendZombieInfos(i32 zid, i32 CID)
{
	// send zombie client informations
	u32 zombCID = ZombCID(zid);

	// send drop first
	// this will pop a chat message
	CNetMsg_Sv_ClientDrop Msg;
	Msg.m_ClientID = zombCID;
	Msg.m_pReason = "";
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL|MSGFLAG_NORECORD, CID);

	// then update
	CNetMsg_Sv_ClientInfo nci;
	nci.m_ClientID = zombCID;
	nci.m_Local = 0;
	nci.m_Team = 0;
	nci.m_pClan = "";
	nci.m_Country = 0;
	nci.m_apSkinPartNames[SKINPART_DECORATION] = "standard";
	nci.m_aUseCustomColors[SKINPART_DECORATION] = 0;
	nci.m_aSkinPartColors[SKINPART_DECORATION] = 0;

	// hands and feets
	i32 brown = PackColor(28, 77, 13);
	i32 red = PackColor(0, 255, 0);
	i32 yellow = PackColor(37, 255, 70); // yellow

	i32 handFeetColor = brown;
	if(m_ZombType[zid] == ZTYPE_TANK) {
		handFeetColor = PackColor(9, 145, 108); // pinkish feet
	}
	if(m_ZombType[zid] == ZTYPE_HUNTER) {
		handFeetColor = PackColor(28, 255, 192);
	}

	// NOTE: is this really needed?
	if(m_ZombBuff[zid]&BUFF_ENRAGED) {
		handFeetColor = red;
	}
	if(m_ZombBuff[zid]&BUFF_ELITE) {
		handFeetColor = yellow;
	}

	nci.m_apSkinPartNames[SKINPART_HANDS] = "standard";
	nci.m_aUseCustomColors[SKINPART_HANDS] = 1;
	nci.m_aSkinPartColors[SKINPART_HANDS] = handFeetColor;
	nci.m_apSkinPartNames[SKINPART_FEET] = "standard";
	nci.m_aUseCustomColors[SKINPART_FEET] = 1;
	nci.m_aSkinPartColors[SKINPART_FEET] = handFeetColor;

	nci.m_apSkinPartNames[SKINPART_EYES] = "zombie_eyes";
	nci.m_aUseCustomColors[SKINPART_EYES] = 0;
	nci.m_aSkinPartColors[SKINPART_EYES] = 0;

	nci.m_aUseCustomColors[SKINPART_MARKING] = 0;
	nci.m_aSkinPartColors[SKINPART_MARKING] = 0;

	if(m_ZombBuff[zid]&BUFF_ENRAGED) {
		nci.m_apSkinPartNames[SKINPART_MARKING] = "enraged";
	}
	else {
		nci.m_apSkinPartNames[SKINPART_MARKING] = "standard";
	}

	nci.m_aUseCustomColors[SKINPART_BODY] = 0;
	nci.m_aSkinPartColors[SKINPART_BODY] = 0;

	if(m_ZombBuff[zid]&BUFF_ELITE) {
		nci.m_aUseCustomColors[SKINPART_BODY] = 1;
		nci.m_aSkinPartColors[SKINPART_BODY] = yellow;
	}

	nci.m_pName = g_ZombName[m_ZombType[zid]];

	switch(m_ZombType[zid]) {
		case ZTYPE_BASIC:
			nci.m_apSkinPartNames[SKINPART_BODY] = zid%2 ? "zombie1" : "zombie2";
			break;

		case ZTYPE_TANK:
			nci.m_apSkinPartNames[SKINPART_BODY] = "tank";
			break;

		case ZTYPE_BOOMER:
			nci.m_apSkinPartNames[SKINPART_BODY] = "boomer";
			break;

		case ZTYPE_BULL:
			nci.m_apSkinPartNames[SKINPART_BODY] = "bull";
			break;

		case ZTYPE_MUDGE:
			nci.m_apSkinPartNames[SKINPART_BODY] = "mudge";
			break;

		case ZTYPE_HUNTER:
			nci.m_apSkinPartNames[SKINPART_BODY] = "hunter";
			break;

		default: break;
	}

	Server()->SendPackMsg(&nci, MSGFLAG_VITAL|MSGFLAG_NORECORD, CID);
}

void CGameControllerZOMB::HandleMovement(u32 zid, const vec2& targetPos, bool targetLOS)
{
	const vec2& pos = m_ZombCharCore[zid].m_Pos;
	CNetObj_PlayerInput& input = m_ZombInput[zid];

	vec2& dest = m_ZombDestination[zid];
	if(targetLOS) {
		dest = targetPos;
	}
	else if(m_ZombPathFindClock[zid] <= 0) {
		dest = PathFind(pos, targetPos);
		m_ZombPathFindClock[zid] = SecondsToTick(0.1f);
	}

	input.m_Direction = -1;
	if(pos.x < dest.x) {
		input.m_Direction = 1;
	}

	// grounded?
	f32 phyzSize = 28.f;
	bool grounded = (GameServer()->Collision()->CheckPoint(pos.x+phyzSize/2, pos.y+phyzSize/2+5) ||
					 GameServer()->Collision()->CheckPoint(pos.x-phyzSize/2, pos.y+phyzSize/2+5));
	for(u32 i = 0; i < MAX_CLIENTS; ++i) {
		CCharacterCore* pCore = GameServer()->m_World.m_Core.m_apCharacters[i];
		if(!pCore) {
			continue;
		}

		if(pos.y < pCore->m_Pos.y &&
		   abs(pos.x - pCore->m_Pos.x) < phyzSize &&
		   abs(pos.y - pCore->m_Pos.y) < (phyzSize*1.5f)) {
			grounded = true;
			break;
		}
	}

	if(grounded) {
		m_ZombAirJumps[zid] = 2;
	}

	// jump
	input.m_Jump = 0;
	if(m_ZombJumpClock[zid] <= 0 && dest.y < pos.y) {
		f32 yDist = abs(dest.y - pos.y);
		if(yDist > 2.f) {
			if(grounded) {
				input.m_Jump = ZOMBIE_GROUNDJUMP;
				m_ZombJumpClock[zid] = SecondsToTick(0.5f);
			}
			else if(m_ZombAirJumps[zid] > 0) {
				input.m_Jump = ZOMBIE_AIRJUMP;
				m_ZombJumpClock[zid] = SecondsToTick(0.5f);
				--m_ZombAirJumps[zid];
			}
		}
		if(yDist > 300.f) {
			input.m_Jump = ZOMBIE_BIGJUMP;
			m_ZombJumpClock[zid] = SecondsToTick(1.0f);
		}
	}
}

void CGameControllerZOMB::HandleHook(u32 zid, f32 targetDist, bool targetLOS)
{
	CNetObj_PlayerInput& input = m_ZombInput[zid];

	if(m_ZombHookClock[zid] <= 0 && targetDist < HOOK_RANGE && targetLOS) {
		input.m_Hook = 1;
	}

	if(m_ZombCharCore[zid].m_HookState == HOOK_GRABBED &&
	   m_ZombCharCore[zid].m_HookedPlayer != m_ZombSurvTarget[zid]) {
		input.m_Hook = 0;
	}

	if(m_ZombCharCore[zid].m_HookState == HOOK_RETRACTED) {
		input.m_Hook = 0;
	}

	if(m_ZombCharCore[zid].m_HookState == HOOK_FLYING) {
		input.m_Hook = 1;
	}

	// keep grabbing target
	if(m_ZombCharCore[zid].m_HookState == HOOK_GRABBED &&
	   m_ZombCharCore[zid].m_HookedPlayer == m_ZombSurvTarget[zid]) {
		++m_ZombHookGrabClock[zid];
		i32 limit = g_ZombGrabLimit[m_ZombType[zid]];
		if(m_ZombBuff[zid]&BUFF_ENRAGED) {
			limit *= 2;
		}

		if(m_ZombHookGrabClock[zid] >= limit) {
			input.m_Hook = 0;
			m_ZombHookGrabClock[zid] = 0;
		}
		else {
			input.m_Hook = 1;
			m_ZombCharCore[zid].m_HookTick = 0;
		}
	}

	if(m_ZombInput[zid].m_Hook == 0) {
		m_ZombHookGrabClock[zid] = 0;
	}

	if(m_ZombCharCore[zid].m_HookState == HOOK_GRABBED) {
		m_ZombHookClock[zid] = g_ZombHookCD[m_ZombType[zid]];
	}
}

void CGameControllerZOMB::HandleBoomer(u32 zid, f32 targetDist, bool targetLOS)
{
	i32 zombCID = ZombCID(zid);
	const vec2& pos = m_ZombCharCore[zid].m_Pos;

	--m_ZombExplodeClock[zid];

	// BOOM
	if(m_ZombExplodeClock[zid] == 0) {
		KillZombie(zid, zombCID);
		i32 dmg = g_ZombAttackDmg[m_ZombType[zid]];
		if(m_ZombBuff[zid]&BUFF_ELITE) {
			dmg *= 2;
		}

		// explosion effect
		GameServer()->CreateExplosion(pos, zombCID, 0, 0);
		GameServer()->CreateSound(pos, SOUND_GRENADE_EXPLODE);

		// damage survivors
		CCharacter *apEnts[MAX_SURVIVORS];
		i32 count = GameServer()->m_World.FindEntities(pos, BOOMER_EXPLOSION_OUTER_RADIUS,
													   (CEntity**)apEnts, MAX_SURVIVORS,
													   CGameWorld::ENTTYPE_CHARACTER);

		f32 radiusDiff = BOOMER_EXPLOSION_OUTER_RADIUS - BOOMER_EXPLOSION_INNER_RADIUS;

		for(i32 s = 0; s < count; ++s) {
			vec2 d = apEnts[s]->GetPos() - pos;
			vec2 n = normalize(d);
			f32 l = length(d);
			f32 factor = 0.2f;
			if(l < BOOMER_EXPLOSION_INNER_RADIUS) {
				factor = 1.f;
			}
			else {
				l -= BOOMER_EXPLOSION_INNER_RADIUS;
				factor = max(0.2f, l / radiusDiff);
			}

			apEnts[s]->TakeDamage(n* 30.f * factor, (i32)(dmg * factor),
					zombCID, WEAPON_GRENADE);
			u32 cid = apEnts[s]->GetPlayer()->GetCID();
			CCharacterCore* pCore = GameServer()->m_World.m_Core.m_apCharacters[cid];
			if(pCore) {
				pCore->m_HookState = HOOK_RETRACTED;
			}
		}

		// knockback zombies
		for(u32 z = 0; z < m_ZombCount; ++z) {
			if(!m_ZombAlive[z] || z == zid) continue;

			vec2 d = m_ZombCharCore[z].m_Pos - pos;
			f32 l = length(d);
			if(l > BOOMER_EXPLOSION_OUTER_RADIUS) {
				continue;
			}

			vec2 n = normalize(d);

			f32 factor = 0.2f;
			if(l < BOOMER_EXPLOSION_INNER_RADIUS) {
				factor = 1.f;
			}
			else {
				l -= BOOMER_EXPLOSION_INNER_RADIUS;
				factor = max(0.2f, l / radiusDiff);
			}

			m_ZombCharCore[z].m_Vel += n * 30.f * factor *
					g_ZombKnockbackMultiplier[m_ZombType[z]];
			m_ZombHookClock[z] = SecondsToTick(1);
			m_ZombInput[z].m_Hook = 0;
		}
	}

	// start the fuse
	if(m_ZombExplodeClock[zid] < 0 && targetDist < BOOMER_EXPLOSION_INNER_RADIUS && targetLOS) {
		m_ZombExplodeClock[zid] = SecondsToTick(0.35f);
		m_ZombInput[zid].m_Hook = 0; // stop hooking to let the survivor a chance to escape
		m_ZombHookClock[zid] = SecondsToTick(1.f);
		// send some love
		GameServer()->SendEmoticon(zombCID, 2);
	}
}

void CGameControllerZOMB::HandleBull(u32 zid, const vec2& targetPos, f32 targetDist, bool targetLOS)
{
	const vec2& pos = m_ZombCharCore[zid].m_Pos;
	i32 zombCID = ZombCID(zid);

	// CHARRRGE
	if(m_ZombChargeClock[zid] <= 0 && targetDist > 100.f && targetLOS && targetDist < 1000.f) {
		m_ZombChargeClock[zid] = BULL_CHARGE_CD;
		f32 chargeDist = targetDist * 2.0f; // overcharge a bit
		m_ZombChargingClock[zid] = SecondsToTick(chargeDist/BULL_CHARGE_SPEED);
		m_ZombChargeVel[zid] = normalize(targetPos - pos) * (BULL_CHARGE_SPEED/SERVER_TICK_SPEED);

		m_ZombAttackTick[zid] = m_Tick; // ninja attack tick

		GameServer()->CreateSound(pos, SOUND_PLAYER_SKID);
		//GameServer()->SendEmoticon(zombCID, 1); // exclamation
		mem_zero(m_ZombChargeHit[zid], sizeof(bool) * MAX_CLIENTS);
	}

	if(m_ZombChargingClock[zid] > 0) {
		m_ZombActiveWeapon[zid] = WEAPON_NINJA;
		m_ZombChargeClock[zid] = BULL_CHARGE_CD; // don't count cd while charging

		i32 dmg = g_ZombAttackDmg[m_ZombType[zid]];
		if(m_ZombBuff[zid]&BUFF_ELITE) {
			dmg *= 2;
		}

		f32 checkRadius = 28.f * 3.f;
		f32 chargeSpeed = length(m_ZombChargeVel[zid]);
		u32 checkCount = max(1, (i32)(chargeSpeed / checkRadius));

		// as it is moving quite fast, check several times along path if needed
		for(u32 c = 0; c < checkCount; ++c) {
			vec2 checkPos = pos + ((m_ZombChargeVel[zid] / (f32)checkCount) * (f32)c);

			CCharacter *apEnts[MAX_SURVIVORS];
			i32 count = GameServer()->m_World.FindEntities(checkPos, checkRadius,
														   (CEntity**)apEnts, MAX_SURVIVORS,
														   CGameWorld::ENTTYPE_CHARACTER);
			for(i32 s = 0; s < count; ++s) {
				i32 cid = apEnts[s]->GetPlayer()->GetCID();

				// hit only once during charge
				if(m_ZombChargeHit[zid][cid]) {
					continue;
				}
				m_ZombChargeHit[zid][cid] = true;

				vec2 d = apEnts[s]->GetPos() - checkPos;
				vec2 n = normalize(d);

				GameServer()->CreateHammerHit(apEnts[s]->GetPos());
				apEnts[s]->TakeDamage(n * BULL_KNOCKBACK_FORCE, dmg,
						zombCID, WEAPON_HAMMER);
				CCharacterCore* pCore = GameServer()->m_World.m_Core.m_apCharacters[cid];
				if(pCore) {
					pCore->m_HookState = HOOK_RETRACTED;
				}
			}

			// knockback zombies
			for(u32 z = 0; z < m_ZombCount; ++z) {
				if(!m_ZombAlive[z] || z == zid) continue;

				// hit only once during charge
				i32 cid = ZombCID(z);
				if(m_ZombChargeHit[zid][cid]) {
					continue;
				}
				m_ZombChargeHit[zid][cid] = true;


				vec2 d = m_ZombCharCore[z].m_Pos - checkPos;
				f32 l = length(d);
				if(l > checkRadius) {
					continue;
				}
				vec2 n = normalize(d);

				m_ZombCharCore[z].m_Vel += n * BULL_KNOCKBACK_FORCE *
						g_ZombKnockbackMultiplier[m_ZombType[z]];
				m_ZombHookClock[z] = SecondsToTick(1);
				m_ZombInput[z].m_Hook = 0;
			}

			// it hit something, reset attack time so it doesn't attack when
			// bumping with charge
			if(count > 0) {
				m_ZombAttackClock[zid] = SecondsToTick(1.f / g_ZombAttackSpeed[m_ZombType[zid]]);
				m_ZombInput[zid].m_Hook = 0;
				m_ZombHookClock[zid] = SecondsToTick(0.25f);
				m_ZombChargingClock[zid] = 1;
			}
		}
	}
	else {
		m_ZombActiveWeapon[zid] = WEAPON_HAMMER;
	}
}

void CGameControllerZOMB::HandleMudge(u32 zid, const vec2& targetPos, f32 targetDist, bool targetLOS)
{
	if(targetLOS && targetDist < 1000.f && m_ZombCharCore[zid].m_HookState != HOOK_GRABBED) {
		m_ZombCharCore[zid].m_HookState = HOOK_GRABBED;
		m_ZombCharCore[zid].m_HookedPlayer = m_ZombSurvTarget[zid];
		GameServer()->CreateSound(targetPos, SOUND_HOOK_ATTACH_PLAYER);
		m_ZombInput[zid].m_Hook = 1;
	}

	// stop moving once it grabs onto someone
	// and pulls a bit stronger
	if(m_ZombCharCore[zid].m_HookState == HOOK_GRABBED) {
		m_ZombInput[zid].m_Jump = 0;
		m_ZombInput[zid].m_Direction = 0;
		vec2 pullDir = normalize(m_ZombCharCore[zid].m_Pos - targetPos);
		CCharacterCore* pTargetCore = GameServer()->m_World.m_Core.m_apCharacters[m_ZombSurvTarget[zid]];
		if(pTargetCore) {
			pTargetCore->m_Vel += pullDir * MUDGE_PULL_STR;
		}
	}
}

void CGameControllerZOMB::HandleHunter(u32 zid, const vec2& targetPos, f32 targetDist, bool targetLOS)
{
	const vec2& pos = m_ZombCharCore[zid].m_Pos;
	i32 zombCID = ZombCID(zid);

	// CHARRRGE
	if(m_ZombChargeClock[zid] <= 0 && targetDist > 100.f && targetDist < 600.f && targetLOS) {
		m_ZombChargeClock[zid] = HUNTER_CHARGE_CD;
		f32 chargeDist = targetDist * 1.5f; // overcharge a bit
		m_ZombChargingClock[zid] = SecondsToTick(chargeDist/HUNTER_CHARGE_SPEED);
		m_ZombChargeVel[zid] = normalize(targetPos - pos) * (HUNTER_CHARGE_SPEED/SERVER_TICK_SPEED);

		m_ZombAttackTick[zid] = m_Tick; // ninja attack tick

		GameServer()->CreateSound(pos, SOUND_NINJA_FIRE);
		//GameServer()->SendEmoticon(zombCID, 10);
		mem_zero(m_ZombChargeHit[zid], sizeof(bool) * MAX_CLIENTS);
	}

	if(m_ZombChargingClock[zid] > 0) {
		m_ZombActiveWeapon[zid] = WEAPON_NINJA;
		m_ZombChargeClock[zid] = HUNTER_CHARGE_CD; // don't count cd while charging

		f32 checkRadius = 28.f * 2.f;
		f32 chargeSpeed = length(m_ZombChargeVel[zid]);
		u32 checkCount = max(1, (i32)(chargeSpeed / checkRadius));

		// as it is moving quite fast, check several times along path if needed
		for(u32 c = 0; c < checkCount; ++c) {
			vec2 checkPos = pos + ((m_ZombChargeVel[zid] / (f32)checkCount) * (f32)c);

			CCharacter *apEnts[MAX_SURVIVORS];
			i32 count = GameServer()->m_World.FindEntities(checkPos, checkRadius,
														   (CEntity**)apEnts, MAX_SURVIVORS,
														   CGameWorld::ENTTYPE_CHARACTER);
			for(i32 s = 0; s < count; ++s) {
				i32 cid = apEnts[s]->GetPlayer()->GetCID();

				// hit only once during charge
				if(m_ZombChargeHit[zid][cid]) {
					continue;
				}
				m_ZombChargeHit[zid][cid] = true;

				GameServer()->CreateHammerHit(apEnts[s]->GetPos());
				apEnts[s]->TakeDamage(vec2(0, 0), HUNTER_CHARGE_DMG, zombCID, WEAPON_NINJA);
			}

			// it hit something, reset attack time so it doesn't attack when
			// hitting with charge
			if(count > 0) {
				m_ZombAttackClock[zid] = SecondsToTick(1.f / g_ZombAttackSpeed[m_ZombType[zid]]);
				GameServer()->CreateSound(pos, SOUND_NINJA_HIT);
			}
		}
	}
	else {
		m_ZombActiveWeapon[zid] = WEAPON_HAMMER;
	}
}

void CGameControllerZOMB::ConZombStart(IConsole::IResult* pResult, void* pUserData)
{
	CGameControllerZOMB *pThis = (CGameControllerZOMB *)pUserData;

	i32 startingWave = 0;
	if(pResult->NumArguments()) {
		startingWave = clamp(pResult->GetInteger(0), 0, MAX_WAVES-1);
	}

	pThis->StartZombGame(startingWave);
}

void CGameControllerZOMB::StartZombGame(u32 startingWave)
{
	m_CurrentWave = startingWave;
	m_SpawnCmdID = 0;
	m_SpawnClock = SecondsToTick(10); // 10s to setup
	m_WaveWaitClock = -1;
	AnnounceWave(m_CurrentWave);
	ChatMessage(">> 10s to setup.");
	StartMatch();
	m_ZombGameState = ZSTATE_WAVE_GAME;
	m_CanPlayersRespawn = true;

	// needed so when SpawnZombie() checks type it sends infos
	// (for the first spawn)
	for(u32 i = 0; i < m_ZombCount; ++i) {
		KillZombie(i, -1);
		m_ZombType[i] = ZTYPE_INVALID;
	}

	m_Seed = RandInt(0, 9999);
}

void CGameControllerZOMB::GameWon()
{
	ChatMessage(">> Game won, good job.");
	EndMatch();
	m_ZombGameState = ZSTATE_NONE;
	m_IsReviveCtfActive = false;

	for(u32 i = 0; i < MAX_SURVIVORS; ++i) {
		if(GameServer()->m_apPlayers[i] && !GameServer()->GetPlayerChar(i)) {
			GameServer()->m_apPlayers[i]->SetTeam(TEAM_RED, false);
		}
	}
}

void CGameControllerZOMB::GameLost()
{
	ChatMessage(">> You LOST.");
	EndMatch();
	m_ZombGameState = ZSTATE_NONE;
	m_IsReviveCtfActive = false;

	for(u32 i = 0; i < MAX_ZOMBS; ++i) {
		if(m_ZombAlive[i]) {
			KillZombie(i, -1);
		}
	}

	for(u32 i = 0; i < MAX_SURVIVORS; ++i) {
		if(GameServer()->m_apPlayers[i] && !GameServer()->GetPlayerChar(i)) {
			GameServer()->m_apPlayers[i]->SetTeam(TEAM_RED, false);
		}
	}

	if(g_Config.m_SvZombAutoRestart) {
		StartZombGame();
	}
}

void CGameControllerZOMB::ChatMessage(const char* msg)
{
	CNetMsg_Sv_Chat chatMsg;
	chatMsg.m_Team = 0;
	chatMsg.m_ClientID = -1;
	chatMsg.m_pMessage = msg;
	Server()->SendPackMsg(&chatMsg, MSGFLAG_VITAL, -1);
}

void CGameControllerZOMB::AnnounceWave(u32 waveID)
{
	CNetMsg_Sv_Chat chatMsg;
	chatMsg.m_Team = 0;
	chatMsg.m_ClientID = -1;
	char msgBuff[256];
	str_format(msgBuff, sizeof(msgBuff), ">> WAVE %d (%d zombies)", waveID + 1,
			   m_WaveSpawnCount[waveID]);
	chatMsg.m_pMessage = msgBuff;
	Server()->SendPackMsg(&chatMsg, MSGFLAG_VITAL, -1);
}

void CGameControllerZOMB::ActivateReviveCtf()
{
	m_RedFlagPos = m_RedFlagSpawn;
	m_BlueFlagPos = m_BlueFlagSpawn[m_Tick%m_BlueFlagSpawnCount];
	m_RedFlagCarrier = -1;
	m_RedFlagVel = vec2(0, 0);
	GameServer()->CreateSound(m_RedFlagPos, SOUND_CTF_RETURN);
	m_IsReviveCtfActive = true;
	m_CanPlayersRespawn = false;

	// if there are more than 2 people playing,
	// spawn flag on a random zombie
	u32 survCount = 0;
	for(u32 i = 0; i < MAX_SURVIVORS; ++i) {
		if(GameServer()->m_apPlayers[i]) {
			++survCount;
		}
	}

	u32 choices[MAX_ZOMBS];
	u32 choiceCount = 0;
	if(survCount > 2) {
		for(u32 i = 0; i < MAX_ZOMBS; ++i) {
			if(m_ZombAlive[i]) {
				choices[choiceCount++] = i;
			}
		}

		if(choiceCount > 0) {
			u32 chosen = choices[m_Tick%choiceCount];
			m_RedFlagCarrier = ZombCID(chosen);
			m_RedFlagPos = m_ZombCharCore[chosen].m_Pos;
		}
	}
}

void CGameControllerZOMB::ReviveSurvivors()
{
	m_CanPlayersRespawn = true;

	for(u32 i = 0; i < MAX_SURVIVORS; ++i) {
		if(GameServer()->m_apPlayers[i] && !GameServer()->GetPlayerChar(i)) {
			GameServer()->m_apPlayers[i]->SetTeam(TEAM_RED, false);
			GameServer()->m_apPlayers[i]->TryRespawn();
		}
	}

	m_CanPlayersRespawn = false;
	m_IsReviveCtfActive = false;
}

enum {
	TK_INVALID = 0,
	TK_OPEN_BRACE,
	TK_CLOSE_BRACE,
	TK_COLON,
	TK_SEMICOLON,
	TK_UNDERSCORE,
	TK_COMMENT,
	TK_IDENTIFIER,
	TK_NUMBER,
	TK_EOS
};

struct Token
{
	const char* at;
	u32 length;
	u8 type;
};

struct Cursor
{
	const char* at;
	u32 line;
};

inline bool IsWhiteSpace(char c)
{
	return (c == ' ' ||
			c == '\n' ||
			c == '\t' ||
			c == '\r');
}

inline bool IsAlpha(char c)
{
	return ((c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z'));
}

inline bool IsNumber(char c)
{
	return (c >= '0' && c <= '9');
}

static Token GetToken(Cursor* pCursor)
{
	while(*pCursor->at && IsWhiteSpace(*pCursor->at)) {
		if(*pCursor->at == '\n') {
			++pCursor->line;
		}
		++pCursor->at;
	}

	Token token;
	token.at = pCursor->at;
	token.length = 1;
	token.type = TK_INVALID;

	switch(*pCursor->at) {
		case '{': token.type = TK_OPEN_BRACE; break;
		case '}': token.type = TK_CLOSE_BRACE; break;
		case ':': token.type = TK_COLON; break;
		case ';': token.type = TK_SEMICOLON; break;
		case '_': token.type = TK_UNDERSCORE; break;
		case 0: token.type = TK_EOS; break;

		case '#': {
			token.type = TK_COMMENT;
			const char* ccur = pCursor->at+1;
			while(*ccur != 0 && *ccur != '\n') {
				++token.length;
				++ccur;
			}
		} break;

		default: {
			if(IsAlpha(*pCursor->at)) {
				token.type = TK_IDENTIFIER;
				const char* ccur = pCursor->at+1;
				while(*ccur != 0 && IsAlpha(*ccur)) {
					++token.length;
					++ccur;
				}
			}
			else if(IsNumber(*pCursor->at)) {
				token.type = TK_NUMBER;
				const char* ccur = pCursor->at+1;
				while(*ccur != 0 && IsNumber(*ccur)) {
					++token.length;
					++ccur;
				}
			}
		} break;
	}

	pCursor->at += token.length;
	return token;
}

static bool ParseZombDecl(Cursor* pCursor, u32* out_pCount, bool* out_pIsElite)
{
	Token token = GetToken(pCursor);

	// _elite
	if(token.type == TK_UNDERSCORE) {
		token = GetToken(pCursor);
		if(token.type == TK_IDENTIFIER && token.length == 5
		   && str_comp_nocase_num(token.at, "elite", 5) == 0) {
			*out_pIsElite = true;
			token = GetToken(pCursor); // :
		}
		else {
			return false;
		}
	}

	if(token.type != TK_COLON) {
		return false;
	}

	token = GetToken(pCursor);
	if(token.type != TK_NUMBER) {
		return false;
	}

	// parse count
	char numBuff[8];
	memcpy(numBuff, token.at, token.length);
	numBuff[token.length] = 0;
	*out_pCount = str_toint(numBuff);

	token = GetToken(pCursor);
	if(token.type != TK_SEMICOLON) {
		return false;
	}

	return true;
}

static bool ParseEnrageDecl(Cursor* pCursor, u32* out_pTime)
{
	Token token = GetToken(pCursor);

	if(token.type != TK_COLON) {
		return false;
	}

	token = GetToken(pCursor);
	if(token.type != TK_NUMBER) {
		return false;
	}

	// parse time
	char numBuff[8];
	memcpy(numBuff, token.at, token.length);
	numBuff[token.length] = 0;
	*out_pTime = str_toint(numBuff);

	token = GetToken(pCursor);
	if(token.type != TK_SEMICOLON) {
		return false;
	}

	return true;
}

bool CGameControllerZOMB::LoadWaveFile(const char* path)
{
	FILE* pWaveFile = fopen(path, "r");
	if(pWaveFile) {
		fseek(pWaveFile, 0, SEEK_END);
		u32 fileSize = ftell(pWaveFile);
		fseek(pWaveFile, 0, SEEK_SET);
		char* fileContents = (char*)mem_alloc(fileSize+1, 1);
		fileSize = fread(fileContents, sizeof(char), fileSize, pWaveFile);
		fileContents[fileSize] = 0;
		fclose(pWaveFile);

		if(!ParseWaveFile(fileContents)) {
			mem_free(fileContents);
			return false;
		}

		mem_free(fileContents);
		return true;
	}

	std_zomb_msg("Error: could not open %s", path);

	return false;
}

bool CGameControllerZOMB::ParseWaveFile(const char* pBuff)
{
	SpawnCmd waveData[MAX_WAVES][MAX_SPAWN_QUEUE];
	u32 waveSpawnCount[MAX_WAVES];
	u32 waveEnrageTime[MAX_WAVES];
	u32 waveCount = 0;

	bool parsing = true;
	Cursor cursor;
	cursor.at = pBuff;
	cursor.line = 1;

	bool insideWave = false;
	i32 waveId = -1;

	while(parsing) {
		Token token = GetToken(&cursor);

		switch(token.type) {
			case TK_OPEN_BRACE: {
				//dbg_zomb_msg("{");
				if(!insideWave) {
					waveId = waveCount++;
					waveEnrageTime[waveId] = DEFAULT_ENRAGE_TIME;
					waveSpawnCount[waveId] = 0;
					insideWave = true;
				}
				else {
					std_zomb_msg("Error: wave parsing error: { not closed (line: %d)", cursor.line);
					return false;
				}
			} break;

			case TK_CLOSE_BRACE: {
				//dbg_zomb_msg("}");
				if(insideWave) {
					if(waveSpawnCount[waveId] == 0) {
						std_zomb_msg("Error: wave parsing error: wave %d is empty (line: %d)", waveId, cursor.line);
						return false;
					}

					insideWave = false;
				}
				else {
					std_zomb_msg("Error: wave parsing error: } not open (line: %d)", cursor.line);
					return false;
				}
			} break;

			case TK_IDENTIFIER: {
				//dbg_zomb_msg("%.*s", token.length, token.at);
				if(insideWave) {
					i32 zombType = -1;
					for(i32 i = 0; i < ZTYPE_MAX; ++i) {
						if(strlen(g_ZombName[i]) == token.length &&
						   str_comp_nocase_num(token.at, g_ZombName[i], token.length) == 0) {
							zombType = i;
							break;
						}
					}

					if(zombType != -1) {
						u32 count = 0;
						bool isElite = false;
						if(ParseZombDecl(&cursor, &count, &isElite)) {
							// add to wave
							for(u32 c = 0; c < count; ++c) {
								waveData[waveId][waveSpawnCount[waveId]++] = SpawnCmd{(u8)zombType,
										isElite};
							}
						}
						else {
							std_zomb_msg("Error: wave parsing error: near %.*s (format is type[_elite]: count;) (line: %d)",
										 token.length, token.at, cursor.line);
							return false;
						}
					}
					else if(token.length == 6 &&
							str_comp_nocase_num(token.at, "enrage", token.length) == 0) {
						u32 enrageTime = 0;
						if(ParseEnrageDecl(&cursor, &enrageTime)) {
							waveEnrageTime[waveId] = enrageTime;
						}
						else {
							std_zomb_msg("Error: wave parsing error: near %.*s (format is enrage: time;) (line: %d)",
										 token.length, token.at, cursor.line);
							return false;
						}

					}
					else {
						std_zomb_msg("Error: wave parsing error: near %.*s (unknown identifer) (line: %d)",
									 token.length, token.at, cursor.line);
						return false;
					}
				}
				else {
					std_zomb_msg("Error: wave parsing error: identifier outside wave block (line: %d)", cursor.line);
					return false;
				}
			} break;

			case TK_EOS: {
				parsing = false;
			} break;
		}
	}

	if(waveCount == 0) {
		std_zomb_msg("Error: wave parsing error: no waves declared");
		return false;
	}

	memcpy(m_WaveData, waveData, sizeof(m_WaveData));
	memcpy(m_WaveSpawnCount, waveSpawnCount, sizeof(m_WaveSpawnCount));
	memcpy(m_WaveEnrageTime, waveEnrageTime, sizeof(m_WaveEnrageTime));
	m_WaveCount = waveCount;

	return true;
}

void CGameControllerZOMB::LoadDefaultWaves()
{
	static const char* defaultWavesFile = {
		"{"
		"	tank: 5;"
		"	tank_elite: 5;"
		"}"
	};

	ParseWaveFile(defaultWavesFile);
}

void CGameControllerZOMB::ConLoadWaveFile(IConsole::IResult* pResult, void* pUserData)
{
	CGameControllerZOMB *pThis = (CGameControllerZOMB *)pUserData;

	if(pResult->NumArguments()) {
		const char* rStr = pResult->GetString(0);
		if(pThis->LoadWaveFile(rStr)) {
			pThis->ChatMessage("-- New waves successfully loaded.");
			memcpy(g_Config.m_SvZombWaveFile, rStr, min(256, (i32)strlen(rStr)));
		}
	}
}

void CGameControllerZOMB::TickReviveCtf()
{
	bool everyoneDead = true;
	for(u32 i = 0; i < MAX_SURVIVORS; ++i) {
		if(GameServer()->GetPlayerChar(i)) {
			everyoneDead = false;
		}
		else if(GameServer()->m_apPlayers[i] &&
				GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS) {
			GameServer()->m_apPlayers[i]->SetTeam(TEAM_SPECTATORS, false);
		}
	}

	if(everyoneDead) {
		GameLost();
		return;
	}

	if(m_RedFlagCarrier == -1) {
		m_RedFlagVel.y += GameServer()->Tuning()->m_Gravity;
		GameServer()->Collision()->MoveBox(&m_RedFlagPos,
										   &m_RedFlagVel,
										   vec2(28.f, 28.f),
										   0.f);

		CCharacter *apEnts[MAX_SURVIVORS];
		i32 count = GameServer()->m_World.FindEntities(m_RedFlagPos, 56.f,
													   (CEntity**)apEnts, MAX_SURVIVORS,
													   CGameWorld::ENTTYPE_CHARACTER);

		for(i32 i = 0; i < count; ++i) {
			m_RedFlagCarrier = apEnts[i]->GetPlayer()->GetCID();
			m_RedFlagVel = vec2(0, 0);
			m_RedFlagPos = apEnts[i]->GetPos();
			GameServer()->CreateSound(m_RedFlagPos, SOUND_CTF_GRAB_PL);
			break;
		}

	}
	else {
		if(IsSurvivor(m_RedFlagCarrier)) {
			m_RedFlagPos = GameServer()->GetPlayerChar(m_RedFlagCarrier)->GetPos();
		}
		else {
			m_RedFlagPos = m_ZombCharCore[m_RedFlagCarrier - MAX_SURVIVORS].m_Pos;
		}

		m_RedFlagVel = vec2(0, 0);
	}

	// capture
	if(distance(m_RedFlagPos, m_BlueFlagPos) < 100.f && IsSurvivor(m_RedFlagCarrier)) {
		GameServer()->CreateSound(m_RedFlagPos, SOUND_CTF_CAPTURE);
		ReviveSurvivors();
	}
}

#ifdef CONF_DEBUG
void CGameControllerZOMB::DebugPathAddPoint(ivec2 p)
{
	if(m_DbgPathLen >= 256) return;
	m_DbgPath[m_DbgPathLen++] = p;
}

void CGameControllerZOMB::DebugLine(ivec2 s, ivec2 e)
{
	if(m_DbgLinesCount >= 256) return;
	u32 id = m_DbgLinesCount++;
	m_DbgLines[id].start = s;
	m_DbgLines[id].end = e;
}
#endif

CGameControllerZOMB::CGameControllerZOMB(CGameContext *pGameServer)
: IGameController(pGameServer)
{
	m_pGameType = "ZOMB";
	m_ZombCount = MAX_ZOMBS;
	m_ZombSpawnPointCount = 0;
	m_SurvSpawnPointCount = 0;
	m_Seed = 1337;

	// get map info
	memcpy(m_MapName, g_Config.m_SvMap, sizeof(g_Config.m_SvMap));
	mem_zero(m_Map, MAX_MAP_SIZE);
	CMapItemLayerTilemap* pGameLayer = GameServer()->Layers()->GameLayer();
	m_MapWidth = pGameLayer->m_Width;
	m_MapHeight = pGameLayer->m_Height;
	CTile* pTiles = (CTile*)(GameServer()->Layers()->Map()->GetData(pGameLayer->m_Data));
	u32 count = m_MapWidth * m_MapHeight;
	for(u32 i = 0; i < count; ++i) {
		if(pTiles[i].m_Index == TILE_SOLID ||
		   pTiles[i].m_Index == TILE_NOHOOK) {
			m_Map[i] = 1;
		}
	}

	// init zombies
	mem_zero(m_ZombAttackTick, sizeof(m_ZombAttackTick));
	mem_zero(m_ZombDmgTick, sizeof(m_ZombDmgTick));
	mem_zero(m_ZombDmgAngle, sizeof(m_ZombDmgAngle));
	mem_zero(m_ZombPathFindClock, sizeof(m_ZombPathFindClock));

	for(u32 i = 0; i < m_ZombCount; ++i) {
		m_ZombCharCore[i].Init(&GameServer()->m_World.m_Core, GameServer()->Collision());
		// needed so when SpawnZombie() checks type it sends infos
		// (for the first spawn)
		m_ZombType[i] = ZTYPE_INVALID;
	}

	GameServer()->Console()->Register("zomb_start", "?i", CFGFLAG_SERVER, ConZombStart,
									  this, "Start a ZOMB game");
	GameServer()->Console()->Register("zomb_load", "s", CFGFLAG_SERVER, ConLoadWaveFile,
									  this, "Load a ZOMB wave file");

	m_ZombGameState = ZSTATE_NONE;

	m_BlueFlagSpawnCount = 0;
	m_IsReviveCtfActive = false;
	m_CanPlayersRespawn = true;

	if(LoadWaveFile(g_Config.m_SvZombWaveFile)) {
		std_zomb_msg("wave file loaded (%d waves).", m_WaveCount);
	}
	else {
		std_zomb_msg("loading default waves.");
		LoadDefaultWaves();
	}

	GameServer()->Console()->ExecuteLine("add_vote \"Start a Zomb game\" zomb_start");
}

void CGameControllerZOMB::Tick()
{
	m_Tick = Server()->Tick();
	IGameController::Tick();

	if(m_ZombGameState == ZSTATE_WAVE_GAME) {
		// wait in between waves
		if(m_WaveWaitClock > 0) {
			--m_WaveWaitClock;
			if(m_WaveWaitClock == 0) {
				++m_CurrentWave;
				m_SpawnClock = SPAWN_INTERVAL;
			}
		}
		else if(m_SpawnClock > 0) {
			--m_SpawnClock;

			if(m_SpawnClock == 0 && m_SpawnCmdID < m_WaveSpawnCount[m_CurrentWave]) {
				for(u32 i = 0; i < m_ZombCount; ++i) {
					if(m_ZombAlive[i]) continue;
					u32 cmdID = m_SpawnCmdID++;
					u8 type = m_WaveData[m_CurrentWave][cmdID].type;
					bool isElite = m_WaveData[m_CurrentWave][cmdID].isElite;
					SpawnZombie(i, type, isElite, m_WaveEnrageTime[m_CurrentWave]);
					break;
				}

				m_SpawnClock = SPAWN_INTERVAL;
			}
		}

		// end of the wave
		if(m_SpawnCmdID == m_WaveSpawnCount[m_CurrentWave]) {
			bool areZombsDead = true;
			for(u32 i = 0; i < m_ZombCount; ++i) {
				if(m_ZombAlive[i]) {
					areZombsDead = false;
					break;
				}
			}

			if(areZombsDead) {
				ReviveSurvivors();
				m_WaveWaitClock = WAVE_WAIT_TIME;
				m_SpawnCmdID = 0;

				if(m_CurrentWave == (m_WaveCount-1)) {
					GameWon();
				}
				else {
					ChatMessage(">> Wave complete.");
					ChatMessage(">> 10s until next wave.");
					AnnounceWave(m_CurrentWave + 1);
				}
			}
		}
	}

	for(u32 i = 0; i < m_ZombCount; ++i) {
		if(!m_ZombAlive[i]) continue;

		// clocks
		--m_ZombPathFindClock[i];
		--m_ZombJumpClock[i];
		--m_ZombAttackClock[i];
		--m_ZombEnrageClock[i];
		--m_ZombHookClock[i];
		--m_ZombChargeClock[i];

		// enrage
		if(m_ZombEnrageClock[i] <= 0 &&
		   (m_ZombBuff[i]&BUFF_ENRAGED) == 0) {
			m_ZombBuff[i] |= BUFF_ENRAGED;
			for(u32 s = 0; s < MAX_SURVIVORS; ++s) {
				if(GameServer()->m_apPlayers[s] && Server()->ClientIngame(s)) {
					SendZombieInfos(i, s);
				}
			}
		}

		// double if enraged
		if(m_ZombBuff[i]&BUFF_ENRAGED) {
			--m_ZombAttackClock[i];
			--m_ZombHookClock[i];
			--m_ZombChargeClock[i];
		}

		vec2 pos = m_ZombCharCore[i].m_Pos;

		// find closest target
		f32 closestDist = -1.f;
		i32 survTargetID = NO_TARGET;
		for(u32 s = 0; s < MAX_SURVIVORS; ++s) {
			CCharacter* pSurvChar = GameServer()->GetPlayerChar(s);
			if(pSurvChar) {
				f32 dist = length(pos - pSurvChar->GetPos());
				if(closestDist < 0.f || dist < closestDist) {
					closestDist = dist;
					survTargetID = s;
				}
			}
		}

		m_ZombSurvTarget[i] = survTargetID;

		if(m_ZombSurvTarget[i] == NO_TARGET) {
			continue;
		}


		vec2 targetPos = GameServer()->GetPlayerChar(m_ZombSurvTarget[i])->GetPos();
		f32 targetDist = distance(pos, targetPos);
		bool targetLOS = (GameServer()->Collision()->IntersectLine(pos, targetPos, 0, 0) == 0);

		HandleMovement(i, targetPos, targetLOS);

		m_ZombInput[i].m_TargetX = targetPos.x - pos.x;
		m_ZombInput[i].m_TargetY = targetPos.y - pos.y;

		// attack!
		if(m_ZombAttackClock[i] <= 0 && targetDist < 56.f) {
			i32 dmg = g_ZombAttackDmg[m_ZombType[i]];
			// double damage if elite
			if(m_ZombBuff[i]&BUFF_ELITE) {
				dmg *= 2;
			}

			SwingHammer(i, dmg, 2.f);
			m_ZombAttackClock[i] = SecondsToTick(1.f / g_ZombAttackSpeed[m_ZombType[i]]);
		}

		HandleHook(i, targetDist, targetLOS);

		// special behaviors
		switch(m_ZombType[i]) {
			case ZTYPE_BOOMER:
				HandleBoomer(i, targetDist, targetLOS);
				break;

			case ZTYPE_BULL:
				HandleBull(i, targetPos, targetDist, targetLOS);
				break;

			case ZTYPE_MUDGE:
				HandleMudge(i, targetPos, targetDist, targetLOS);
				break;

			case ZTYPE_HUNTER:
				HandleHunter(i, targetPos, targetDist, targetLOS);
				break;

			default: break;
		}
	}

	// core actual move
	for(u32 i = 0; i < m_ZombCount; ++i) {
		if(!m_ZombAlive[i]) continue;
		m_ZombCharCore[i].m_Input = m_ZombInput[i];

		// handle jump
		m_ZombCharCore[i].m_Jumped = 0;
		i32 jumpTriggers = 0;
		if(m_ZombCharCore[i].m_Input.m_Jump) {
			f32 jumpStr = 14.f;
			if(m_ZombCharCore[i].m_Input.m_Jump == ZOMBIE_BIGJUMP) {
				jumpStr = 22.f;
				jumpTriggers |= COREEVENTFLAG_AIR_JUMP;
				m_ZombCharCore[i].m_Jumped |= 3;
			}
			else if(m_ZombCharCore[i].m_Input.m_Jump == ZOMBIE_AIRJUMP) {
				jumpTriggers |= COREEVENTFLAG_AIR_JUMP;
				m_ZombCharCore[i].m_Jumped |= 3;
			}
			else {
				jumpTriggers |= COREEVENTFLAG_GROUND_JUMP;
				m_ZombCharCore[i].m_Jumped |= 1;
			}

			m_ZombCharCore[i].m_Vel.y = -jumpStr;
			m_ZombCharCore[i].m_Input.m_Jump = 0;
		}

		m_ZombCharCore[i].Tick(true);

		m_ZombCharCore[i].m_TriggeredEvents |= jumpTriggers;

		// smooth out small movement
		f32 xDist = abs(m_ZombDestination[i].x - m_ZombCharCore[i].m_Pos.x);
		if(xDist < 5.f) {
			m_ZombCharCore[i].m_Vel.x = (m_ZombDestination[i].x - m_ZombCharCore[i].m_Pos.x);
		}

		if(m_ZombChargingClock[i] > 0) {
			--m_ZombChargingClock[i];

			// reset input except from firing
			i32 doFire = m_ZombInput[i].m_Fire;
			m_ZombInput[i] = CNetObj_PlayerInput();
			m_ZombInput[i].m_Fire = doFire;

			// don't move naturally
			m_ZombCharCore[i].m_Vel = vec2(0, 0);

			vec2 chargeVel = m_ZombChargeVel[i];
			GameServer()->Collision()->MoveBox(&m_ZombCharCore[i].m_Pos,
											   &chargeVel,
											   vec2(28.f, 28.f),
											   0.f);
		}

		m_ZombCharCore[i].Move();

		// predict charge
		if(m_ZombChargingClock[i] > 0) {
			m_ZombCharCore[i].m_Vel = m_ZombChargeVel[i];
		}
	}

	if(m_IsReviveCtfActive && m_ZombGameState != ZSTATE_NONE) {
		TickReviveCtf();
	}
}

void CGameControllerZOMB::Snap(i32 SnappingClientID)
{
	if(!IsSurvivor(SnappingClientID)) {
		return;
	}

	IGameController::Snap(SnappingClientID);

	// send zombie player and character infos
	for(u32 i = 0; i < m_ZombCount; ++i) {
		if(!m_ZombAlive[i]) {
			continue;
		}

		u32 zombCID = ZombCID(i);

		CNetObj_PlayerInfo *pPlayerInfo = (CNetObj_PlayerInfo *)Server()->SnapNewItem(NETOBJTYPE_PLAYERINFO,
												zombCID, sizeof(CNetObj_PlayerInfo));
		if(!pPlayerInfo) {
			dbg_zomb_msg("Error: failed to SnapNewItem(NETOBJTYPE_PLAYERINFO)");
			return;
		}

		pPlayerInfo->m_PlayerFlags = PLAYERFLAG_READY;
		pPlayerInfo->m_Latency = zombCID;
		pPlayerInfo->m_Score = zombCID;

		CNetObj_Character *pCharacter = (CNetObj_Character *)Server()->SnapNewItem(NETOBJTYPE_CHARACTER,
													zombCID, sizeof(CNetObj_Character));
		if(!pCharacter) {
			dbg_zomb_msg("Error: failed to SnapNewItem(NETOBJTYPE_CHARACTER)");
			return;
		}

		pCharacter->m_Tick = m_Tick;

		// eyes
		pCharacter->m_Emote = EMOTE_NORMAL;
		if(m_ZombBuff[i]&BUFF_ENRAGED) {
			pCharacter->m_Emote = EMOTE_SURPRISE;
		}
		// boomer found his soulmate
		if(m_ZombType[i] == ZTYPE_BOOMER && m_ZombExplodeClock[i] > 0) {
			pCharacter->m_Emote = EMOTE_HAPPY;
		}

		pCharacter->m_TriggeredEvents = m_ZombCharCore[i].m_TriggeredEvents;
		pCharacter->m_Weapon = m_ZombActiveWeapon[i];
		pCharacter->m_AttackTick = m_ZombAttackTick[i];

		m_ZombCharCore[i].Write(pCharacter);

		i32 xOffset = 0.f;
		if(m_ZombBuff[i]&BUFF_HEALING && m_ZombBuff[i]&BUFF_ARMORED) {
			xOffset = 16.f;
		}

		// buffs
		if(m_ZombBuff[i]&BUFF_HEALING) {
			CNetObj_Pickup *pPickup = (CNetObj_Pickup *)Server()->SnapNewItem(NETOBJTYPE_PICKUP,
											256+zombCID, sizeof(CNetObj_Pickup));
			if(!pPickup)
				return;

			pPickup->m_X = m_ZombCharCore[i].m_Pos.x - xOffset;
			pPickup->m_Y = m_ZombCharCore[i].m_Pos.y - 72.f;
			pPickup->m_Type = PICKUP_HEALTH;
		}

		if(m_ZombBuff[i]&BUFF_ARMORED) {
			CNetObj_Pickup *pPickup = (CNetObj_Pickup *)Server()->SnapNewItem(NETOBJTYPE_PICKUP,
											512+zombCID, sizeof(CNetObj_Pickup));
			if(!pPickup)
				return;

			pPickup->m_X = m_ZombCharCore[i].m_Pos.x + xOffset;
			pPickup->m_Y = m_ZombCharCore[i].m_Pos.y - 72.f;
			pPickup->m_Type = PICKUP_ARMOR;
		}
	}

	// revive ctf
	if(m_IsReviveCtfActive) {
		CNetObj_Flag *pFlag = (CNetObj_Flag *)Server()->SnapNewItem(NETOBJTYPE_FLAG,
																	TEAM_RED, sizeof(CNetObj_Flag));
		if(pFlag) {
			pFlag->m_X = m_RedFlagPos.x;
			pFlag->m_Y = m_RedFlagPos.y;
			pFlag->m_Team = TEAM_RED;
		}

		pFlag = (CNetObj_Flag *)Server()->SnapNewItem(NETOBJTYPE_FLAG,
													  TEAM_BLUE, sizeof(CNetObj_Flag));
		if(pFlag) {
			pFlag->m_X = m_BlueFlagPos.x;
			pFlag->m_Y = m_BlueFlagPos.y;
			pFlag->m_Team = TEAM_BLUE;
		}

		CNetObj_GameDataFlag *pGameDataFlag = (CNetObj_GameDataFlag *)
				Server()->SnapNewItem(NETOBJTYPE_GAMEDATAFLAG, 0, sizeof(CNetObj_GameDataFlag));
		if(pGameDataFlag) {
			pGameDataFlag->m_FlagCarrierBlue = FLAG_ATSTAND;
			if(m_RedFlagCarrier == -1) {
				pGameDataFlag->m_FlagCarrierRed = FLAG_ATSTAND;
			}
			else {
				pGameDataFlag->m_FlagCarrierRed = m_RedFlagCarrier;
			}
		}
	}

#ifdef CONF_DEBUG
	i32 tick = Server()->Tick();
	u32 laserID = 0;

	// debug path
	for(u32 i = 1; i < m_DbgPathLen; ++i) {
		CNetObj_Laser *pObj = (CNetObj_Laser*)Server()->SnapNewItem(NETOBJTYPE_LASER,
									 laserID++, sizeof(CNetObj_Laser));
		if(!pObj)
			return;

		pObj->m_X = m_DbgPath[i-1].x * 32 + 16;
		pObj->m_Y = m_DbgPath[i-1].y * 32 + 16;
		pObj->m_FromX = m_DbgPath[i].x * 32 + 16;
		pObj->m_FromY = m_DbgPath[i].y * 32 + 16;
		pObj->m_StartTick = tick;
	}

	// debug lines
	for(u32 i = 1; i < m_DbgLinesCount; ++i) {
		CNetObj_Laser *pObj = (CNetObj_Laser*)Server()->SnapNewItem(NETOBJTYPE_LASER,
									 laserID++, sizeof(CNetObj_Laser));
		if(!pObj)
			return;

		pObj->m_X = m_DbgLines[i].start.x * 32 + 16;
		pObj->m_Y = m_DbgLines[i].start.y * 32 + 16;
		pObj->m_FromX = m_DbgLines[i].end.x * 32 + 16;
		pObj->m_FromY = m_DbgLines[i].end.y * 32 + 16;
		pObj->m_StartTick = tick;
	}
#endif
}

void CGameControllerZOMB::OnPlayerConnect(CPlayer* pPlayer)
{
	IGameController::OnPlayerConnect(pPlayer);

	for(u32 i = 0; i < MAX_ZOMBS; ++i) {
		if(!m_ZombAlive[i]) continue;
		SendZombieInfos(i, pPlayer->GetCID());
	}

	pPlayer->SetTeam(TEAM_RED, false);
}

bool CGameControllerZOMB::IsFriendlyFire(int ClientID1, int ClientID2) const
{
	// both survivors
	if(IsSurvivor(ClientID1) && IsSurvivor(ClientID2)) {
		return true;
	}

	// both zombies
	if(IsZombie(ClientID1) && IsZombie(ClientID2)) {
		return true;
	}

	return false;
}

bool CGameControllerZOMB::OnEntity(int Index, vec2 Pos)
{
	bool r = IGameController::OnEntity(Index, Pos);

	if(Index == ENTITY_SPAWN_BLUE || Index == ENTITY_SPAWN) {
		m_ZombSpawnPoint[m_ZombSpawnPointCount++] = Pos;
	}
	if(Index == ENTITY_SPAWN_RED || Index == ENTITY_SPAWN) {
		m_SurvSpawnPoint[m_SurvSpawnPointCount++] = Pos;
	}

	if(Index == ENTITY_FLAGSTAND_RED) {
		m_RedFlagSpawn = Pos;
	}

	if(Index == ENTITY_FLAGSTAND_BLUE) {
		m_BlueFlagSpawn[m_BlueFlagSpawnCount++] = Pos;
	}

	return r;
}

bool CGameControllerZOMB::HasEnoughPlayers() const
{
	// get rid of that annoying warmup message
	return true;
}

bool CGameControllerZOMB::CanSpawn(int Team, vec2* pPos) const
{
	if(Team == TEAM_SPECTATORS) {
		return false;
	}

	if(!m_CanPlayersRespawn && m_ZombGameState != ZSTATE_NONE) {
		return false;
	}

	u32 chosen = (m_Tick%m_SurvSpawnPointCount);
	CCharacter *aEnts[MAX_SURVIVORS];
	for(u32 i = 0; i < m_SurvSpawnPointCount; ++i) {
		i32 count = GameServer()->m_World.FindEntities(m_SurvSpawnPoint[i], 28.f, (CEntity**)aEnts,
													   MAX_SURVIVORS, CGameWorld::ENTTYPE_CHARACTER);
		if(count == 0) {
			chosen = i;
			break;
		}
	}

	*pPos = m_SurvSpawnPoint[chosen];
	return true;
}

int CGameControllerZOMB::OnCharacterDeath(CCharacter* pVictim, CPlayer* pKiller, int Weapon)
{
	if(!m_IsReviveCtfActive && m_ZombGameState != ZSTATE_NONE) {
		ActivateReviveCtf();
	}

	if(m_RedFlagCarrier == pVictim->GetPlayer()->GetCID()) {
		m_RedFlagCarrier = -1;
		GameServer()->CreateSound(m_RedFlagPos, SOUND_CTF_DROP);
	}

	return IGameController::OnCharacterDeath(pVictim, pKiller, Weapon);
}

void CGameControllerZOMB::ZombTakeDmg(i32 CID, vec2 Force, i32 Dmg, int From, i32 Weapon)
{
	// don't take damage from other zombies
	if(IsZombie(From)) {
		return;
	}

	u32 zid = CID - MAX_SURVIVORS;

	// make sure damage indicators don't group together
	++m_ZombDmgAngle[zid];
	if(Server()->Tick() < m_ZombDmgTick[zid]+25) {
		GameServer()->CreateDamageInd(m_ZombCharCore[zid].m_Pos, m_ZombDmgAngle[zid]*0.25f, Dmg);
	}
	else {
		m_ZombDmgAngle[zid] = 0;
		GameServer()->CreateDamageInd(m_ZombCharCore[zid].m_Pos, 0, Dmg);
	}
	m_ZombDmgTick[zid] = Server()->Tick();

	m_ZombHealth[zid] -= Dmg;
	if(m_ZombHealth[zid] <= 0) {
		KillZombie(zid, -1);
	}

	m_ZombCharCore[zid].m_Vel += Force * g_ZombKnockbackMultiplier[m_ZombType[zid]];
}
