#include <base/color.h>
#include <base/math.h>
#include <base/system.h>
#include <engine/shared/config.h>
#include "teecomp.h"

vec3 CTeecompUtils::GetTeamColor(int ForTeam, int LocalTeam, int Color1, int Color2, int Method)
{
	vec3 c1((Color1>>16)/255.0f, ((Color1>>8)&0xff)/255.0f, (Color1&0xff)/255.0f);
	vec3 c2((Color2>>16)/255.0f, ((Color2>>8)&0xff)/255.0f, (Color2&0xff)/255.0f);

	// Team based Colors or spectating
	if(!Method || LocalTeam == -1)
	{
		if(ForTeam == 0)
			return c1;
		return c2;
	}

	// Enemy based Colors
	if(ForTeam == LocalTeam)
		return c1;
	return c2;
}

// using GetTeamColor instead in gameclient.cpp now
/* int CTeecompUtils::GetTeamColorInt(int ForTeam, int LocalTeam, int Color1, int Color2, int Method)
{
	// Team based Colors or spectating
	if(!Method || LocalTeam == -1)
	{
		if(ForTeam == 0)
			return Color1;
		return Color2;
	}

	// Enemy based Colors
	if(ForTeam == LocalTeam)
		return Color1;
	return Color2;
} */

bool CTeecompUtils::GetForcedSkinName(int ForTeam, int LocalTeam, const char*& pSkinName)
{
	// Team based Colors or spectating
	if(!g_Config.m_TcForcedSkinsMethod || LocalTeam == -1)
	{
		if(ForTeam == 0)
		{
			pSkinName = g_Config.m_TcForcedSkin1;
			return g_Config.m_TcForceSkinTeam1;
		}
		pSkinName = g_Config.m_TcForcedSkin2;
		return g_Config.m_TcForceSkinTeam2;
	}

	// Enemy based Colors
	if(ForTeam == LocalTeam)
	{
		pSkinName = g_Config.m_TcForcedSkin1;
		return g_Config.m_TcForceSkinTeam1;
	}
	pSkinName = g_Config.m_TcForcedSkin2;
	return g_Config.m_TcForceSkinTeam2;
}

bool CTeecompUtils::GetForceDmColors(int ForTeam, int LocalTeam)
{
	if(!g_Config.m_TcColoredTeesMethod || LocalTeam == -1)
	{
		if(ForTeam == 0)
			return g_Config.m_TcDmColorsTeam1;
		return g_Config.m_TcDmColorsTeam2;
	}

	if(ForTeam == LocalTeam)
		return g_Config.m_TcDmColorsTeam1;
	return g_Config.m_TcDmColorsTeam2;
}

void CTeecompUtils::ResetConfig()
{
	#define MACRO_CONFIG_INT(Name,ScriptName,Def,Min,Max,Save,Desc) g_Config.m_##Name = Def;
	#define MACRO_CONFIG_STR(Name,ScriptName,Len,Def,Save,Desc) str_copy(g_Config.m_##Name, Def, Len);
	#include "../teecomp_vars.h"
	#undef MACRO_CONFIG_INT
	#undef MACRO_CONFIG_STR
}

const char* CTeecompUtils::RgbToName(int rgb)
{
	vec3 rgb_v((rgb>>16)/255.0f, ((rgb>>8)&0xff)/255.0f, (rgb&0xff)/255.0f);
	vec3 hsl = RgbToHsl(rgb_v);

	if(hsl.l < 0.2f)
		return "Black";
	if(hsl.l > 0.9f)
		return "White";
	if(hsl.s < 0.1f)
		return "Gray";
	if(hsl.h < 20)
		return "Red";
	if(hsl.h < 45)
		return "Orange";
	if(hsl.h < 70)
		return "Yellow";
	if(hsl.h < 155)
		return "Green";
	if(hsl.h < 260)
		return "Blue";
	if(hsl.h < 335)
		return "Purple";
	return "Red";
}

const char* CTeecompUtils::TeamColorToName(int rgb)
{
	vec3 rgb_v((rgb>>16)/255.0f, ((rgb>>8)&0xff)/255.0f, (rgb&0xff)/255.0f);
	vec3 hsl = RgbToHsl(rgb_v);

	if(hsl.l < 0.2f)
		return "black team";
	if(hsl.l > 0.9f)
		return "white team";
	if(hsl.s < 0.1f)
		return "gray team";
	if(hsl.h < 20)
		return "red team";
	if(hsl.h < 45)
		return "orange team";
	if(hsl.h < 70)
		return "yellow team";
	if(hsl.h < 155)
		return "green team";
	if(hsl.h < 260)
		return "blue team";
	if(hsl.h < 335)
		return "purple team";
	return "red team";
}
