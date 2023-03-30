/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016      Spike

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sv_main.c -- server main program

#include "quakedef.h"

server_t	sv;
server_static_t	svs;

static char	localmodels[MAX_MODELS][8];	// inline model names for precache

int		sv_protocol = PROTOCOL_RMQ; //johnfitz

extern cvar_t nomonsters;

static cvar_t sv_netsort = {"sv_netsort", "1", CVAR_NONE};

//============================================================================

void SV_CalcStats(client_t *client, int *statsi, float *statsf, const char **statss)
{
	size_t i;
	edict_t *ent = client->edict;
	//FIXME: string stats!
	int items;
	eval_t *val = GetEdictFieldValue(ent, qcvm->extfields.items2);
	if (val)
		items = (int)ent->v.items | ((int)val->_float << 23);
	else
		items = (int)ent->v.items | ((int)pr_global_struct->serverflags << 28);

	memset(statsi, 0, sizeof(*statsi)*MAX_CL_STATS);
	memset(statsf, 0, sizeof(*statsf)*MAX_CL_STATS);
	memset((void*)statss, 0, sizeof(*statss)*MAX_CL_STATS);
	statsf[STAT_HEALTH] = ent->v.health;
//	statsf[STAT_FRAGS] = ent->v.frags;	//obsolete
	statsi[STAT_WEAPON] = SV_ModelIndex(PR_GetString(ent->v.weaponmodel));
	//if ((unsigned int)statsi[STAT_WEAPON] >= client->limit_models)
	//	statsi[STAT_WEAPON] = 0;
	statsf[STAT_AMMO] = ent->v.currentammo;
	statsf[STAT_ARMOR] = ent->v.armorvalue;
	statsf[STAT_WEAPONFRAME] = ent->v.weaponframe;
	statsf[STAT_SHELLS] = ent->v.ammo_shells;
	statsf[STAT_NAILS] = ent->v.ammo_nails;
	statsf[STAT_ROCKETS] = ent->v.ammo_rockets;
	statsf[STAT_CELLS] = ent->v.ammo_cells;
	statsf[STAT_ACTIVEWEAPON] = ent->v.weapon;	//sent in a way that does NOT depend upon the current mod...

	//FIXME: add support for clientstat/globalstat qc builtins.

	for (i = 0; i < sv.numcustomstats; i++)
	{
		eval_t *eval = sv.customstats[i].ptr;
		if (!eval)
			eval = GetEdictFieldValue(ent, sv.customstats[i].fld);

		switch(sv.customstats[i].type)
		{
		case ev_ext_integer:
			statsi[sv.customstats[i].idx] = eval->_int;
			break;
		case ev_entity:
			statsi[sv.customstats[i].idx] = NUM_FOR_EDICT(PROG_TO_EDICT(eval->edict));
			break;
		case ev_float:
			statsf[sv.customstats[i].idx] = eval->_float;
			break;
		case ev_vector:
			statsf[sv.customstats[i].idx+0] = eval->vector[0];
			statsf[sv.customstats[i].idx+1] = eval->vector[1];
			statsf[sv.customstats[i].idx+2] = eval->vector[2];
			break;
		case ev_string:		//not supported in this build... send with svcfte_updatestatstring on change, which is annoying.
			statss[sv.customstats[i].idx] = PR_GetString(eval->string);
			break;
		case ev_void:		//nothing...
		case ev_field:		//panic! everyone panic!
		case ev_function:	//doesn't make much sense
		case ev_pointer:	//doesn't make sense
		default:
			break;
		}
	}
}

/*
===============
SV_Protocol_f
===============
*/
void SV_Protocol_f (void)
{
	int i;

	switch (Cmd_Argc())
	{
	case 1:
		Con_Printf ("\"sv_protocol\" is \"%i\"\n", sv_protocol);
		break;
	case 2:
		i = atoi(Cmd_Argv(1));
		if (i != PROTOCOL_NETQUAKE && i != PROTOCOL_FITZQUAKE && i != PROTOCOL_RMQ)
			Con_Printf ("sv_protocol must be %i or %i or %i\n", PROTOCOL_NETQUAKE, PROTOCOL_FITZQUAKE, PROTOCOL_RMQ);
		else
		{
			sv_protocol = i;
			if (sv.active)
				Con_Printf ("changes will not take effect until the next level load.\n");
		}
		break;
	default:
		Con_SafePrintf ("usage: sv_protocol <protocol>\n");
		break;
	}
}

/*
===============
SV_Init
===============
*/
void SV_Init (void)
{
	int		i;
	const char	*p;
	extern	cvar_t	sv_maxvelocity;
	extern	cvar_t	sv_gravity;
	extern	cvar_t	sv_nostep;
	extern	cvar_t	sv_freezenonclients;
	extern	cvar_t	sv_friction;
	extern	cvar_t	sv_edgefriction;
	extern	cvar_t	sv_stopspeed;
	extern	cvar_t	sv_maxspeed;
	extern	cvar_t	sv_accelerate;
	extern	cvar_t	sv_idealpitchscale;
	extern	cvar_t	sv_aim;
	extern	cvar_t	sv_altnoclip; //johnfitz
	extern	cvar_t	sv_gameplayfix_random;
	extern	cvar_t	sv_autoload;
	extern	cvar_t	sv_autosave;
	extern	cvar_t	sv_autosave_interval;

	Cvar_RegisterVariable (&sv_maxvelocity);
	Cvar_RegisterVariable (&sv_gravity);
	Cvar_RegisterVariable (&sv_friction);
	Cvar_SetCallback (&sv_gravity, Host_Callback_Notify);
	Cvar_SetCallback (&sv_friction, Host_Callback_Notify);
	Cvar_RegisterVariable (&sv_edgefriction);
	Cvar_RegisterVariable (&sv_stopspeed);
	Cvar_RegisterVariable (&sv_maxspeed);
	Cvar_SetCallback (&sv_maxspeed, Host_Callback_Notify);
	Cvar_RegisterVariable (&sv_accelerate);
	Cvar_RegisterVariable (&sv_idealpitchscale);
	Cvar_RegisterVariable (&sv_aim);
	Cvar_RegisterVariable (&sv_nostep);
	Cvar_RegisterVariable (&sv_freezenonclients);
	Cvar_RegisterVariable (&pr_checkextension);
	Cvar_RegisterVariable (&sv_altnoclip); //johnfitz
	Cvar_RegisterVariable (&sv_gameplayfix_random);
	Cvar_RegisterVariable (&sv_netsort);
	Cvar_RegisterVariable (&sv_autoload);
	Cvar_RegisterVariable (&sv_autosave);
	Cvar_RegisterVariable (&sv_autosave_interval);

	Cmd_AddCommand ("sv_protocol", &SV_Protocol_f); //johnfitz

	for (i=0 ; i<MAX_MODELS ; i++)
		sprintf (localmodels[i], "*%i", i);

	i = COM_CheckParm ("-protocol");
	if (i && i < com_argc - 1)
		sv_protocol = atoi (com_argv[i + 1]);
	switch (sv_protocol)
	{
	case PROTOCOL_NETQUAKE:
		p = "NetQuake";
		break;
	case PROTOCOL_FITZQUAKE:
		p = "FitzQuake";
		break;
	case PROTOCOL_RMQ:
		p = "RMQ";
		break;
	default:
		Sys_Error ("Bad protocol version request %i. Accepted values: %i, %i, %i.",
				sv_protocol, PROTOCOL_NETQUAKE, PROTOCOL_FITZQUAKE, PROTOCOL_RMQ);
		return; /* silence compiler */
	}
	Sys_Printf ("Server using protocol %i (%s)\n", sv_protocol, p);
}

/*
=============================================================================

EVENT MESSAGES

=============================================================================
*/

/*
==================
SV_StartParticle

Make sure the event gets sent to all clients
==================
*/
void SV_StartParticle (vec3_t org, vec3_t dir, int color, int count)
{
	int		i, v;

	if (sv.datagram.cursize > MAX_DATAGRAM-18)
		return;
	MSG_WriteByte (&sv.datagram, svc_particle);
	MSG_WriteCoord (&sv.datagram, org[0], sv.protocolflags);
	MSG_WriteCoord (&sv.datagram, org[1], sv.protocolflags);
	MSG_WriteCoord (&sv.datagram, org[2], sv.protocolflags);
	for (i=0 ; i<3 ; i++)
	{
		v = dir[i]*16;
		if (v > 127)
			v = 127;
		else if (v < -128)
			v = -128;
		MSG_WriteChar (&sv.datagram, v);
	}
	MSG_WriteByte (&sv.datagram, count);
	MSG_WriteByte (&sv.datagram, color);
}

/*
==================
SV_StartSound

Each entity can have eight independant sound sources, like voice,
weapon, feet, etc.

Channel 0 is an auto-allocate channel, the others override anything
already running on that entity/channel pair.

An attenuation of 0 will play full volume everywhere in the level.
Larger attenuations will drop off.  (max 4 attenuation)

==================
*/
void SV_StartSound (edict_t *entity, int channel, const char *sample, int volume, float attenuation)
{
	int			sound_num, ent;
	int			i, field_mask;

	if (volume < 0 || volume > 255)
		Host_Error ("SV_StartSound: volume = %i", volume);

	if (attenuation < 0 || attenuation > 4)
		Host_Error ("SV_StartSound: attenuation = %f", attenuation);

	if (channel < 0 || channel > 7)
		Host_Error ("SV_StartSound: channel = %i", channel);

	if (sv.datagram.cursize > MAX_DATAGRAM-21)
		return;

// find precache number for sound
	for (sound_num = 1; sound_num < MAX_SOUNDS && sv.sound_precache[sound_num]; sound_num++)
	{
		if (!strcmp(sample, sv.sound_precache[sound_num]))
			break;
	}

	if (sound_num == MAX_SOUNDS || !sv.sound_precache[sound_num])
	{
		Con_Printf ("SV_StartSound: %s not precached\n", sample);
		return;
	}

	ent = NUM_FOR_EDICT(entity);

	field_mask = 0;
	if (volume != DEFAULT_SOUND_PACKET_VOLUME)
		field_mask |= SND_VOLUME;
	if (attenuation != DEFAULT_SOUND_PACKET_ATTENUATION)
		field_mask |= SND_ATTENUATION;

	//johnfitz -- PROTOCOL_FITZQUAKE
	if (ent >= 8192)
	{
		if (sv.protocol == PROTOCOL_NETQUAKE)
			return; //don't send any info protocol can't support
		field_mask |= SND_LARGEENTITY;
	}
	if (sound_num >= 256 || channel >= 8)
	{
		if (sv.protocol == PROTOCOL_NETQUAKE)
			return; //don't send any info protocol can't support
		field_mask |= SND_LARGESOUND;
	}
	//johnfitz

	if (sv.datagram.cursize > MAX_DATAGRAM-21)
		return;

// directed messages go only to the entity the are targeted on
	MSG_WriteByte (&sv.datagram, svc_sound);
	MSG_WriteByte (&sv.datagram, field_mask);
	if (field_mask & SND_VOLUME)
		MSG_WriteByte (&sv.datagram, volume);
	if (field_mask & SND_ATTENUATION)
		MSG_WriteByte (&sv.datagram, attenuation*64);

	//johnfitz -- PROTOCOL_FITZQUAKE
	if (field_mask & SND_LARGEENTITY)
	{
		MSG_WriteShort (&sv.datagram, ent);
		MSG_WriteByte (&sv.datagram, channel);
	}
	else
		MSG_WriteShort (&sv.datagram, (ent<<3) | channel);
	if (field_mask & SND_LARGESOUND)
		MSG_WriteShort (&sv.datagram, sound_num);
	else
		MSG_WriteByte (&sv.datagram, sound_num);
	//johnfitz

	for (i = 0; i < 3; i++)
		MSG_WriteCoord (&sv.datagram, entity->v.origin[i]+0.5*(entity->v.mins[i]+entity->v.maxs[i]), sv.protocolflags);
}

/*
==================
SV_LocalSound - for 2021 rerelease
==================
*/
void SV_LocalSound (client_t *client, const char *sample)
{
	int	sound_num, field_mask;

	for (sound_num = 1; sound_num < MAX_SOUNDS && sv.sound_precache[sound_num]; sound_num++)
	{
		if (!strcmp(sample, sv.sound_precache[sound_num]))
			break;
	}
	if (sound_num == MAX_SOUNDS || !sv.sound_precache[sound_num])
	{
		Con_Printf ("SV_LocalSound: %s not precached\n", sample);
		return;
	}

	field_mask = 0;
	if (sound_num >= 256)
	{
		if (sv.protocol == PROTOCOL_NETQUAKE)
			return;
		field_mask = SND_LARGESOUND;
	}

	if (client->message.cursize > client->message.maxsize-4)
		return;

	MSG_WriteByte (&client->message, svc_localsound);
	MSG_WriteByte (&client->message, field_mask);
	if (field_mask & SND_LARGESOUND)
		MSG_WriteShort (&client->message, sound_num);
	else
		MSG_WriteByte (&client->message, sound_num);
}

/*
==============================================================================

CLIENT SPAWNING

==============================================================================
*/

static qboolean SV_IsLocalClient (client_t *client)
{
	return Q_strcmp (NET_QSocketGetAddressString (client->netconnection), "LOCAL") == 0;
}

/*
================
SV_SendServerinfo

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each server load.
================
*/
void SV_SendServerinfo (client_t *client)
{
	const char		**s;
	char			message[2048];
	int				i; //johnfitz

	MSG_WriteByte (&client->message, svc_print);
	sprintf (message, "%c\nFITZQUAKE %1.2f SERVER (%i CRC)\n", 2, FITZQUAKE_VERSION, qcvm->crc); //johnfitz -- include fitzquake version
	MSG_WriteString (&client->message,message);

	MSG_WriteByte (&client->message, svc_serverinfo);
	MSG_WriteLong (&client->message, sv.protocol); //johnfitz -- sv.protocol instead of PROTOCOL_VERSION
	
	if (sv.protocol == PROTOCOL_RMQ)
	{
		// mh - now send protocol flags so that the client knows the protocol features to expect
		MSG_WriteLong (&client->message, sv.protocolflags);
	}
	
	MSG_WriteByte (&client->message, svs.maxclients);

	if (!coop.value && deathmatch.value)
		MSG_WriteByte (&client->message, GAME_DEATHMATCH);
	else
		MSG_WriteByte (&client->message, GAME_COOP);

	MSG_WriteString (&client->message, PR_GetString(qcvm->edicts->v.message));

	//johnfitz -- only send the first 256 model and sound precaches if protocol is 15
	for (i = 1, s = sv.model_precache+1; *s; s++,i++)
		if (sv.protocol != PROTOCOL_NETQUAKE || i < 256)
			MSG_WriteString (&client->message, *s);
	MSG_WriteByte (&client->message, 0);

	for (i = 1, s = sv.sound_precache+1; *s; s++, i++)
		if (sv.protocol != PROTOCOL_NETQUAKE || i < 256)
			MSG_WriteString (&client->message, *s);
	MSG_WriteByte (&client->message, 0);
	//johnfitz

// send music
	MSG_WriteByte (&client->message, svc_cdtrack);
	MSG_WriteByte (&client->message, qcvm->edicts->v.sounds);
	MSG_WriteByte (&client->message, qcvm->edicts->v.sounds);

// set view
	MSG_WriteByte (&client->message, svc_setview);
	MSG_WriteShort (&client->message, NUM_FOR_EDICT(client->edict));

	MSG_WriteByte (&client->message, svc_signonnum);
	MSG_WriteByte (&client->message, 1);

	client->sendsignon = PRESPAWN_FLUSH;
	client->spawned = false;		// need prespawn, spawn, etc
}

/*
================
SV_ConnectClient

Initializes a client_t for a new net connection.  This will only be called
once for a player each game, not once for each level change.
================
*/
void SV_ConnectClient (int clientnum)
{
	edict_t			*ent;
	client_t		*client;
	int				edictnum;
	struct qsocket_s *netconnection;
	int				i;
	float			spawn_parms[NUM_SPAWN_PARMS];

	client = svs.clients + clientnum;

	Con_DPrintf ("Client %s connected\n", NET_QSocketGetAddressString(client->netconnection));

	edictnum = clientnum+1;

	ent = EDICT_NUM(edictnum);

// set up the client_t
	netconnection = client->netconnection;

	if (sv.loadgame)
		memcpy (spawn_parms, client->spawn_parms, sizeof(spawn_parms));
	memset (client, 0, sizeof(*client));
	client->netconnection = netconnection;

	strcpy (client->name, "unconnected");
	client->active = true;
	client->spawned = false;
	client->edict = ent;
	client->message.data = client->msgbuf;
	client->message.maxsize = sizeof(client->msgbuf);
	client->message.allowoverflow = true;		// we can catch it

	if (sv.loadgame)
		memcpy (client->spawn_parms, spawn_parms, sizeof(spawn_parms));
	else
	{
	// call the progs to get default spawn parms for the new client
		PR_ExecuteProgram (pr_global_struct->SetNewParms);
		for (i=0 ; i<NUM_SPAWN_PARMS ; i++)
			client->spawn_parms[i] = (&pr_global_struct->parm1)[i];
	}

	SV_SendServerinfo (client);
}


/*
===================
SV_CheckForNewClients

===================
*/
void SV_CheckForNewClients (void)
{
	struct qsocket_s	*ret;
	int				i;

//
// check for new connections
//
	while (1)
	{
		ret = NET_CheckNewConnections ();
		if (!ret)
			break;

	//
	// init a new client structure
	//
		for (i=0 ; i<svs.maxclients ; i++)
			if (!svs.clients[i].active)
				break;
		if (i == svs.maxclients)
			Sys_Error ("Host_CheckForNewClients: no free clients");

		svs.clients[i].netconnection = ret;
		SV_ConnectClient (i);

		net_activeconnections++;
	}
}


/*
===============================================================================

FRAME UPDATES

===============================================================================
*/

/*
==================
SV_ClearDatagram

==================
*/
void SV_ClearDatagram (void)
{
	SZ_Clear (&sv.datagram);
}

/*
=============================================================================

The PVS must include a small area around the client to allow head bobbing
or other small motion on the client side.  Otherwise, a bob might cause an
entity that should be visible to not show up, especially when the bob
crosses a waterline.

=============================================================================
*/

static int	fatbytes;
static byte	*fatpvs;
static int	fatpvs_capacity;

void SV_AddToFatPVS (vec3_t org, mnode_t *node, qmodel_t *worldmodel) //johnfitz -- added worldmodel as a parameter
{
	int		i;
	byte	*pvs;
	mplane_t	*plane;
	float	d;

	while (1)
	{
	// if this is a leaf, accumulate the pvs bits
		if (node->contents < 0)
		{
			if (node->contents != CONTENTS_SOLID)
			{
				pvs = Mod_LeafPVS ( (mleaf_t *)node, worldmodel); //johnfitz -- worldmodel as a parameter
				for (i=0 ; i<fatbytes ; i++)
					fatpvs[i] |= pvs[i];
			}
			return;
		}

		plane = node->plane;
		d = DotProduct (org, plane->normal) - plane->dist;
		if (d > 8)
			node = node->children[0];
		else if (d < -8)
			node = node->children[1];
		else
		{	// go down both
			SV_AddToFatPVS (org, node->children[0], worldmodel); //johnfitz -- worldmodel as a parameter
			node = node->children[1];
		}
	}
}

/*
=============
SV_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.
=============
*/
byte *SV_FatPVS (vec3_t org, qmodel_t *worldmodel) //johnfitz -- added worldmodel as a parameter
{
	fatbytes = (worldmodel->numleafs+7)>>3; // ericw -- was +31, assumed to be a bug/typo
	if (fatpvs == NULL || fatbytes > fatpvs_capacity)
	{
		fatpvs_capacity = fatbytes;
		fatpvs = (byte *) realloc (fatpvs, fatpvs_capacity);
		if (!fatpvs)
			Sys_Error ("SV_FatPVS: realloc() failed on %d bytes", fatpvs_capacity);
	}
	
	Q_memset (fatpvs, 0, fatbytes);
	SV_AddToFatPVS (org, worldmodel->nodes, worldmodel); //johnfitz -- worldmodel as a parameter
	return fatpvs;
}

/*
=============
SV_EdictInPVS
=============
*/
qboolean SV_EdictInPVS (edict_t *test, byte *pvs)
{
	int i;
	for (i = 0 ; i < test->num_leafs ; i++)
		if (pvs[test->leafnums[i] >> 3] & (1 << (test->leafnums[i] & 7)))
			return true;
	return false;
}

/*
=============
SV_VisibleToClient -- johnfitz

PVS test encapsulated in a nice function
=============
*/
qboolean SV_VisibleToClient (edict_t *client, edict_t *test, qmodel_t *worldmodel)
{
	byte	*pvs;
	vec3_t	org;

	VectorAdd (client->v.origin, client->v.view_ofs, org);
	pvs = SV_FatPVS (org, worldmodel);

	return SV_EdictInPVS (test, pvs);
}

//=============================================================================

#define MAX_NET_EDICTS 65536

static uint16_t		net_edicts[MAX_NET_EDICTS];
static byte			net_edict_dists[MAX_NET_EDICTS];
static int			net_edict_bins[256];
static uint16_t		net_edicts_sorted[MAX_NET_EDICTS];

/*
=============
SV_WriteEntitiesToClient

=============
*/
void SV_WriteEntitiesToClient (edict_t	*clent, sizebuf_t *msg)
{
	int		e, i, j, numents;
	int		bits;
	byte	*pvs;
	vec3_t	org, forward, right, up;
	float	miss, dist, size;
	eval_t	*val;
	edict_t	*ent;

// find the client's PVS
	VectorAdd (clent->v.origin, clent->v.view_ofs, org);
	pvs = SV_FatPVS (org, sv.worldmodel);

// find the client's orientation
	AngleVectors (clent->v.v_angle, forward, right, up);

// reset sorting bins
	memset (net_edict_bins, 0, sizeof (net_edict_bins));

// add clent
	if (sv_netsort.value)
	{
		net_edicts[0] = NUM_FOR_EDICT (clent);
		net_edict_dists[0] = 0;
		net_edict_bins[0] = 1;
	}
	else
		net_edicts_sorted[0] = NUM_FOR_EDICT (clent);
	numents = 1;

// add all other entities that touch the pvs
	ent = NEXT_EDICT(qcvm->edicts);
	for (e=1 ; e<qcvm->num_edicts ; e++, ent = NEXT_EDICT(ent))
	{
		if (ent != clent)	// clent already added before the loop
		{
			// ignore ents without visible models
			if (!ent->v.modelindex || !PR_GetString(ent->v.model)[0])
				continue;

			//johnfitz -- don't send model>255 entities if protocol is 15
			if (sv.protocol == PROTOCOL_NETQUAKE && (int)ent->v.modelindex & 0xFF00)
				continue;

			// ignore if not touching a PV leaf
			for (i=0 ; i < ent->num_leafs ; i++)
				if (pvs[ent->leafnums[i] >> 3] & (1 << (ent->leafnums[i]&7) ))
					break;
			
			// ericw -- added ent->num_leafs < MAX_ENT_LEAFS condition.
			//
			// if ent->num_leafs == MAX_ENT_LEAFS, the ent is visible from too many leafs
			// for us to say whether it's in the PVS, so don't try to vis cull it.
			// this commonly happens with rotators, because they often have huge bboxes
			// spanning the entire map, or really tall lifts, etc.
			if (i == ent->num_leafs && ent->num_leafs < MAX_ENT_LEAFS)
				continue;		// not visible

			if (sv_netsort.value)
			{
				// compute ent bbox size and distance from org to the closest point in ent's bbox
				dist = size = 0.f;
				for (i=0 ; i<3 ; i++)
				{
					float delta = CLAMP (ent->v.absmin[i], org[i], ent->v.absmax[i]) - org[i];
					dist += delta * delta;
					delta = ent->v.absmax[i] - ent->v.absmin[i];
					size += delta * delta;
				}
				size = q_max (1.f, size);

				// use scaled square root of (distance/size) as sort key
				dist = 8.f * sqrt (sqrt (dist/size));
				net_edict_dists[numents] = (int) q_min (dist, 255.f);
				net_edicts[numents] = e;

				// compute max distance along forward axis
				dist = 0.f;
				for (i=0 ; i<3 ; i++)
					dist += ((forward[i] < 0.f ? ent->v.absmin[i] : ent->v.absmax[i]) - org[i]) * forward[i];
				if (dist < 0.f)
					net_edict_dists[numents] |= 128; // deprioritize entities behind the client

				net_edict_bins[net_edict_dists[numents]]++;
			}
			else
				net_edicts_sorted[numents] = e;

			if (++numents == MAX_NET_EDICTS)
				break;
		}
		else
			continue;
	}

	if (sv_netsort.value)
	{
		// compute bin offsets
		e = 0;
		for (i=0 ; i<countof(net_edict_bins) ; i++)
		{
			int tmp = net_edict_bins[i];
			net_edict_bins[i] = e;
			e += tmp;
		}

		// generate sorted list
		for (e=0 ; e<numents ; e++)
			net_edicts_sorted[net_edict_bins[net_edict_dists[e]]++] = net_edicts[e];
	}

// send entities (closest first)
	for (j=0 ; j<numents ; j++)
	{
		e = net_edicts_sorted[j];
		ent = EDICT_NUM (e);

		// johnfitz -- max size for protocol 15 is 18 bytes, not 16 as originally
		// assumed here.  And, for protocol 85 the max size is actually 24 bytes.
		// For float coords and angles the limit is 40.
		// FIXME: Use tighter limit according to protocol flags and send bits.
		if (msg->cursize + 40 > msg->maxsize)
		{
			//johnfitz -- less spammy overflow message
			if (!dev_overflows.packetsize || dev_overflows.packetsize + CONSOLE_RESPAM_TIME < realtime )
			{
				Con_Printf ("Packet overflow!\n");
				dev_overflows.packetsize = realtime;
			}
			goto stats;
			//johnfitz
		}

// send an update
		bits = 0;

		for (i=0 ; i<3 ; i++)
		{
			miss = ent->v.origin[i] - ent->baseline.origin[i];
			if ( miss < -0.1 || miss > 0.1 )
				bits |= U_ORIGIN1<<i;
		}

		if ( ent->v.angles[0] != ent->baseline.angles[0] )
			bits |= U_ANGLE1;

		if ( ent->v.angles[1] != ent->baseline.angles[1] )
			bits |= U_ANGLE2;

		if ( ent->v.angles[2] != ent->baseline.angles[2] )
			bits |= U_ANGLE3;

		if (ent->v.movetype == MOVETYPE_STEP)
			bits |= U_STEP;	// don't mess up the step animation

		if (ent->baseline.colormap != ent->v.colormap)
			bits |= U_COLORMAP;

		if (ent->baseline.skin != ent->v.skin)
			bits |= U_SKIN;

		if (ent->baseline.frame != ent->v.frame)
			bits |= U_FRAME;

		if ((ent->baseline.effects ^ (int)ent->v.effects) & qcvm->effects_mask)
			bits |= U_EFFECTS;

		if (ent->baseline.modelindex != ent->v.modelindex)
			bits |= U_MODEL;

		//johnfitz -- alpha
		// TODO: find a cleaner place to put this code
		val = GetEdictFieldValueByName(ent, "alpha");
		if (val)
			ent->alpha = ENTALPHA_ENCODE(val->_float);

		//don't send invisible entities unless they have effects
		if (ent->alpha == ENTALPHA_ZERO && !((int)ent->v.effects & qcvm->effects_mask))
			continue;
		//johnfitz

		val = GetEdictFieldValueByName(ent, "scale");
		if (val)
			ent->scale = ENTSCALE_ENCODE(val->_float);
		else
			ent->scale = ENTSCALE_DEFAULT;

		//johnfitz -- PROTOCOL_FITZQUAKE
		if (sv.protocol != PROTOCOL_NETQUAKE)
		{

			if (ent->baseline.alpha != ent->alpha) bits |= U_ALPHA;
			if (ent->baseline.scale != ent->scale) bits |= U_SCALE;
			if (bits & U_FRAME && (int)ent->v.frame & 0xFF00) bits |= U_FRAME2;
			if (bits & U_MODEL && (int)ent->v.modelindex & 0xFF00) bits |= U_MODEL2;
			if (ent->sendinterval) bits |= U_LERPFINISH;
			if (bits >= 65536) bits |= U_EXTEND1;
			if (bits >= 16777216) bits |= U_EXTEND2;
		}
		//johnfitz

		if (e >= 256)
			bits |= U_LONGENTITY;

		if (bits >= 256)
			bits |= U_MOREBITS;

	//
	// write the message
	//
		MSG_WriteByte (msg, bits | U_SIGNAL);

		if (bits & U_MOREBITS)
			MSG_WriteByte (msg, bits>>8);

		//johnfitz -- PROTOCOL_FITZQUAKE
		if (bits & U_EXTEND1)
			MSG_WriteByte(msg, bits>>16);
		if (bits & U_EXTEND2)
			MSG_WriteByte(msg, bits>>24);
		//johnfitz

		if (bits & U_LONGENTITY)
			MSG_WriteShort (msg,e);
		else
			MSG_WriteByte (msg,e);

		if (bits & U_MODEL)
			MSG_WriteByte (msg,	ent->v.modelindex);
		if (bits & U_FRAME)
			MSG_WriteByte (msg, ent->v.frame);
		if (bits & U_COLORMAP)
			MSG_WriteByte (msg, ent->v.colormap);
		if (bits & U_SKIN)
			MSG_WriteByte (msg, ent->v.skin);
		if (bits & U_EFFECTS)
			MSG_WriteByte (msg, (int)ent->v.effects & qcvm->effects_mask);
		if (bits & U_ORIGIN1)
			MSG_WriteCoord (msg, ent->v.origin[0], sv.protocolflags);
		if (bits & U_ANGLE1)
			MSG_WriteAngle(msg, ent->v.angles[0], sv.protocolflags);
		if (bits & U_ORIGIN2)
			MSG_WriteCoord (msg, ent->v.origin[1], sv.protocolflags);
		if (bits & U_ANGLE2)
			MSG_WriteAngle(msg, ent->v.angles[1], sv.protocolflags);
		if (bits & U_ORIGIN3)
			MSG_WriteCoord (msg, ent->v.origin[2], sv.protocolflags);
		if (bits & U_ANGLE3)
			MSG_WriteAngle(msg, ent->v.angles[2], sv.protocolflags);

		//johnfitz -- PROTOCOL_FITZQUAKE
		if (bits & U_ALPHA)
			MSG_WriteByte(msg, ent->alpha);
		if (bits & U_SCALE)
			MSG_WriteByte(msg, ent->scale);
		if (bits & U_FRAME2)
			MSG_WriteByte(msg, (int)ent->v.frame >> 8);
		if (bits & U_MODEL2)
			MSG_WriteByte(msg, (int)ent->v.modelindex >> 8);
		if (bits & U_LERPFINISH)
			MSG_WriteByte(msg, (byte)(Q_rint((ent->v.nextthink-qcvm->time)*255)));
		//johnfitz
	}

	//johnfitz -- devstats
stats:
	if (msg->cursize > 1024 && dev_peakstats.packetsize <= 1024)
		Con_DWarning ("%i byte packet exceeds standard limit of 1024 (max = %d).\n", msg->cursize, msg->maxsize);
	dev_stats.packetsize = msg->cursize;
	dev_peakstats.packetsize = q_max(msg->cursize, dev_peakstats.packetsize);
	//johnfitz
}

/*
=============
SV_CleanupEnts

=============
*/
void SV_CleanupEnts (void)
{
	int		e;
	edict_t	*ent;

	ent = NEXT_EDICT(qcvm->edicts);
	for (e=1 ; e<qcvm->num_edicts ; e++, ent = NEXT_EDICT(ent))
	{
		ent->v.effects = (int)ent->v.effects & ~EF_MUZZLEFLASH;
	}
}

/*
==================
SV_WriteClientdataToMessage

==================
*/
void SV_WriteClientdataToMessage (edict_t *ent, sizebuf_t *msg)
{
	int		bits;
	int		i;
	edict_t	*other;
	int		items;
	eval_t	*val;

//
// send a damage message
//
	if (ent->v.dmg_take || ent->v.dmg_save)
	{
		other = PROG_TO_EDICT(ent->v.dmg_inflictor);
		MSG_WriteByte (msg, svc_damage);
		MSG_WriteByte (msg, ent->v.dmg_save);
		MSG_WriteByte (msg, ent->v.dmg_take);
		for (i=0 ; i<3 ; i++)
			MSG_WriteCoord (msg, other->v.origin[i] + 0.5*(other->v.mins[i] + other->v.maxs[i]), sv.protocolflags );

		ent->v.dmg_take = 0;
		ent->v.dmg_save = 0;
	}

//
// send the current viewpos offset from the view entity
//
	SV_SetIdealPitch ();		// how much to look up / down ideally

// a fixangle might get lost in a dropped packet.  Oh well.
	if ( ent->v.fixangle )
	{
		MSG_WriteByte (msg, svc_setangle);
		for (i=0 ; i < 3 ; i++)
			MSG_WriteAngle (msg, ent->v.angles[i], sv.protocolflags );
		ent->v.fixangle = 0;
	}

	bits = 0;

	if (ent->v.view_ofs[2] != DEFAULT_VIEWHEIGHT)
		bits |= SU_VIEWHEIGHT;

	if (ent->v.idealpitch)
		bits |= SU_IDEALPITCH;

// stuff the sigil bits into the high bits of items for sbar, or else
// mix in items2
	val = GetEdictFieldValueByName(ent, "items2");

	if (val)
		items = (int)ent->v.items | ((int)val->_float << 23);
	else
		items = (int)ent->v.items | ((int)pr_global_struct->serverflags << 28);

	bits |= SU_ITEMS;

	if ( (int)ent->v.flags & FL_ONGROUND)
		bits |= SU_ONGROUND;

	if ( ent->v.waterlevel >= 2)
		bits |= SU_INWATER;

	for (i=0 ; i<3 ; i++)
	{
		if (ent->v.punchangle[i])
			bits |= (SU_PUNCH1<<i);
		if (ent->v.velocity[i])
			bits |= (SU_VELOCITY1<<i);
	}

	if (ent->v.weaponframe)
		bits |= SU_WEAPONFRAME;

	if (ent->v.armorvalue)
		bits |= SU_ARMOR;

//	if (ent->v.weapon)
	  bits |= SU_WEAPON;

	//johnfitz -- PROTOCOL_FITZQUAKE
	if (sv.protocol != PROTOCOL_NETQUAKE)
	{
		if (bits & SU_WEAPON && SV_ModelIndex(PR_GetString(ent->v.weaponmodel)) & 0xFF00) bits |= SU_WEAPON2;
		if ((int)ent->v.armorvalue & 0xFF00) bits |= SU_ARMOR2;
		if ((int)ent->v.currentammo & 0xFF00) bits |= SU_AMMO2;
		if ((int)ent->v.ammo_shells & 0xFF00) bits |= SU_SHELLS2;
		if ((int)ent->v.ammo_nails & 0xFF00) bits |= SU_NAILS2;
		if ((int)ent->v.ammo_rockets & 0xFF00) bits |= SU_ROCKETS2;
		if ((int)ent->v.ammo_cells & 0xFF00) bits |= SU_CELLS2;
		if (bits & SU_WEAPONFRAME && (int)ent->v.weaponframe & 0xFF00) bits |= SU_WEAPONFRAME2;
		if (bits & SU_WEAPON && ent->alpha != ENTALPHA_DEFAULT) bits |= SU_WEAPONALPHA; //for now, weaponalpha = client entity alpha
		if (bits >= 65536) bits |= SU_EXTEND1;
		if (bits >= 16777216) bits |= SU_EXTEND2;
	}
	//johnfitz

// send the data

	MSG_WriteByte (msg, svc_clientdata);
	MSG_WriteShort (msg, bits);

	//johnfitz -- PROTOCOL_FITZQUAKE
	if (bits & SU_EXTEND1) MSG_WriteByte(msg, bits>>16);
	if (bits & SU_EXTEND2) MSG_WriteByte(msg, bits>>24);
	//johnfitz

	if (bits & SU_VIEWHEIGHT)
		MSG_WriteChar (msg, ent->v.view_ofs[2]);

	if (bits & SU_IDEALPITCH)
		MSG_WriteChar (msg, ent->v.idealpitch);

	for (i=0 ; i<3 ; i++)
	{
		if (bits & (SU_PUNCH1<<i))
			MSG_WriteChar (msg, ent->v.punchangle[i]);
		if (bits & (SU_VELOCITY1<<i))
			MSG_WriteChar (msg, ent->v.velocity[i]/16);
	}

// [always sent]	if (bits & SU_ITEMS)
	MSG_WriteLong (msg, items);

	if (bits & SU_WEAPONFRAME)
		MSG_WriteByte (msg, ent->v.weaponframe);
	if (bits & SU_ARMOR)
		MSG_WriteByte (msg, ent->v.armorvalue);
	if (bits & SU_WEAPON)
		MSG_WriteByte (msg, SV_ModelIndex(PR_GetString(ent->v.weaponmodel)));

	MSG_WriteShort (msg, ent->v.health);
	MSG_WriteByte (msg, ent->v.currentammo);
	MSG_WriteByte (msg, ent->v.ammo_shells);
	MSG_WriteByte (msg, ent->v.ammo_nails);
	MSG_WriteByte (msg, ent->v.ammo_rockets);
	MSG_WriteByte (msg, ent->v.ammo_cells);

	if (standard_quake)
	{
		MSG_WriteByte (msg, ent->v.weapon);
	}
	else
	{
		for(i=0;i<32;i++)
		{
			if ( ((int)ent->v.weapon) & (1<<i) )
			{
				MSG_WriteByte (msg, i);
				break;
			}
		}
	}

	//johnfitz -- PROTOCOL_FITZQUAKE
	if (bits & SU_WEAPON2)
		MSG_WriteByte (msg, SV_ModelIndex(PR_GetString(ent->v.weaponmodel)) >> 8);
	if (bits & SU_ARMOR2)
		MSG_WriteByte (msg, (int)ent->v.armorvalue >> 8);
	if (bits & SU_AMMO2)
		MSG_WriteByte (msg, (int)ent->v.currentammo >> 8);
	if (bits & SU_SHELLS2)
		MSG_WriteByte (msg, (int)ent->v.ammo_shells >> 8);
	if (bits & SU_NAILS2)
		MSG_WriteByte (msg, (int)ent->v.ammo_nails >> 8);
	if (bits & SU_ROCKETS2)
		MSG_WriteByte (msg, (int)ent->v.ammo_rockets >> 8);
	if (bits & SU_CELLS2)
		MSG_WriteByte (msg, (int)ent->v.ammo_cells >> 8);
	if (bits & SU_WEAPONFRAME2)
		MSG_WriteByte (msg, (int)ent->v.weaponframe >> 8);
	if (bits & SU_WEAPONALPHA)
		MSG_WriteByte (msg, ent->alpha); //for now, weaponalpha = client entity alpha
	//johnfitz

	// Hack: Alkaline 1.1 uses bit flags to store the active weapon,
	// but we only send the stat as a byte, which can lead to truncation.
	// If we detect this, re-send the stat separately (as a 32-bit int).
	if ((byte)ent->v.weapon != (int)ent->v.weapon && msg->cursize + 6 <= msg->maxsize)
	{
		MSG_WriteByte (msg, svc_updatestat);
		MSG_WriteByte (msg, STAT_ACTIVEWEAPON);
		MSG_WriteLong (msg, (int)ent->v.weapon);
	}
}

/*
=======================
SV_SendClientDatagram
=======================
*/
qboolean SV_SendClientDatagram (client_t *client)
{
	byte		buf[MAX_DATAGRAM];
	sizebuf_t	msg;

	msg.data = buf;
	msg.maxsize = sizeof(buf);
	msg.cursize = 0;

	//johnfitz -- if client is nonlocal, use smaller max size so packets aren't fragmented
	if (Q_strcmp(NET_QSocketGetAddressString(client->netconnection), "LOCAL") != 0)
		msg.maxsize = DATAGRAM_MTU;
	//johnfitz

	MSG_WriteByte (&msg, svc_time);
	MSG_WriteFloat (&msg, qcvm->time);

// add the client specific data to the datagram
	SV_WriteClientdataToMessage (client->edict, &msg);

	SV_WriteEntitiesToClient (client->edict, &msg);

// copy the server datagram if there is space
	if (msg.cursize + sv.datagram.cursize < msg.maxsize)
		SZ_Write (&msg, sv.datagram.data, sv.datagram.cursize);

// send the datagram
	if (NET_SendUnreliableMessage (client->netconnection, &msg) == -1)
	{
		SV_DropClient (true);// if the message couldn't send, kick off
		return false;
	}

	return true;
}

/*
=======================
SV_WriteStats

TODO: group multiple stats in a single stuffcmd, the client already supports this
=======================
*/
void SV_WriteStats (client_t *client)
{
	int			statsi[MAX_CL_STATS];
	float		statsf[MAX_CL_STATS];
	const char	*statss[MAX_CL_STATS];
	int			i;

	SV_CalcStats (client, statsi, statsf, statss);

	for (i = 0; i < MAX_CL_STATS; i++)
	{
		//small cleanup
		if (!statsi[i])
			statsi[i] =	statsf[i];
		else
			statsf[i] =	0;//statsi[i];

		if (i >= STAT_NONCLIENT && (statsi[i] != client->oldstats_i[i] || statsf[i] != client->oldstats_f[i]))
		{
			client->oldstats_i[i] = statsi[i];
			client->oldstats_f[i] = statsf[i];

			if ((double)statsi[i] != statsf[i] && statsf[i])
			{	//didn't round nicely, so send as a float
				MSG_WriteByte (&client->message, svc_stufftext);
				MSG_WriteString (&client->message, va ("//st %i %g\n", i, statsf[i]));
			}
			else
			{
				if (i < MAX_CL_BASE_STATS)
				{
					MSG_WriteByte (&client->message, svc_updatestat);
					MSG_WriteByte (&client->message, i);
					MSG_WriteLong (&client->message, statsi[i]);
				}
				else
				{
					MSG_WriteByte (&client->message, svc_stufftext);
					MSG_WriteString (&client->message, va ("//st %i %i\n", i, statsi[i]));
				}
			}
		}

		if (statss[i] || client->oldstats_s[i])
		{
			const char *os = client->oldstats_s[i];
			const char *ns = statss[i];
			if (!ns)	ns="";
			if (!os)	os="";
			if (strcmp(os,ns))
			{
				free(client->oldstats_s[i]);
				client->oldstats_s[i] = strdup(ns);

				MSG_WriteByte (&client->message, svc_stufftext);
				MSG_WriteString (&client->message, va ("//sts %i \"%s\"\n", i, ns));
			}
		}
	}
}

/*
=======================
SV_WriteUnderwaterOverride
=======================
*/
void SV_WriteUnderwaterOverride (client_t *client)
{
	if (!client->edict->sendforcewater)
		return;
	client->edict->sendforcewater = false;
	MSG_WriteByte (&client->message, svc_stufftext);
	MSG_WriteString (&client->message, va ("//v_water %i\n", client->edict->forcewater));
}


/*
=======================
SV_UpdateToReliableMessages
=======================
*/
void SV_UpdateToReliableMessages (void)
{
	int			i, j;
	client_t *client;

// check for changes to be sent over the reliable streams
	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
	{
		if (host_client->old_frags != host_client->edict->v.frags)
		{
			for (j=0, client = svs.clients ; j<svs.maxclients ; j++, client++)
			{
				if (!client->active)
					continue;
				MSG_WriteByte (&client->message, svc_updatefrags);
				MSG_WriteByte (&client->message, i);
				MSG_WriteShort (&client->message, host_client->edict->v.frags);
			}

			host_client->old_frags = host_client->edict->v.frags;
		}
	}

	for (j=0, client = svs.clients ; j<svs.maxclients ; j++, client++)
	{
		if (!client->active)
			continue;
		SV_WriteStats (client);
		SV_WriteUnderwaterOverride (client);
		SZ_Write (&client->message, sv.reliable_datagram.data, sv.reliable_datagram.cursize);
	}

	SZ_Clear (&sv.reliable_datagram);
}


/*
=======================
SV_SendNop

Send a nop message without trashing or sending the accumulated client
message buffer
=======================
*/
void SV_SendNop (client_t *client)
{
	sizebuf_t	msg;
	byte		buf[4];

	msg.data = buf;
	msg.maxsize = sizeof(buf);
	msg.cursize = 0;

	MSG_WriteChar (&msg, svc_nop);

	if (NET_SendUnreliableMessage (client->netconnection, &msg) == -1)
		SV_DropClient (true);	// if the message couldn't send, kick off
	client->last_message = realtime;
}

/*
=======================
SV_SendClientMessages
=======================
*/
void SV_SendClientMessages (void)
{
	int			i;

// update frags, names, etc
	SV_UpdateToReliableMessages ();

// build individual updates
	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
	{
		if (!host_client->active)
			continue;

		if (host_client->spawned)
		{
			if (!SV_SendClientDatagram (host_client))
				continue;
		}
		else
		{
		// the player isn't totally in the game yet
		// send small keepalive messages if too much time has passed
		// send a full message when the next signon stage has been requested
		// some other message data (name changes, etc) may accumulate
		// between signon stages
			if (!host_client->sendsignon)
			{
				if (realtime - host_client->last_message > 5)
					SV_SendNop (host_client);
				continue;	// don't send out non-signon messages
			}
			if (host_client->sendsignon == PRESPAWN_SIGNONBUFS)
			{
				qboolean local = SV_IsLocalClient (host_client);
				while (host_client->signonidx < sv.num_signon_buffers)
				{
					sizebuf_t *signon = sv.signon_buffers[host_client->signonidx];
					if (host_client->message.cursize + signon->cursize > host_client->message.maxsize)
						break;
					SZ_Write (&host_client->message, signon->data, signon->cursize);
					host_client->signonidx++;
					// only send multiple buffers at once when playing locally,
					// otherwise we send one signon at a time to avoid overflowing
					// the datagram buffer for clients using a lower limit (e.g. 32000 in QS)
					if (!local)
						break;
				}
				if (host_client->signonidx == sv.num_signon_buffers)
					host_client->sendsignon = PRESPAWN_SIGNONMSG;
			}
			if (host_client->sendsignon == PRESPAWN_SIGNONMSG)
			{
				if (host_client->message.cursize + 2 < host_client->message.maxsize)
				{
					MSG_WriteByte (&host_client->message, svc_signonnum);
					MSG_WriteByte (&host_client->message, 2);
					host_client->sendsignon = PRESPAWN_FLUSH;
				}
			}
		}

		// check for an overflowed message.  Should only happen
		// on a very fucked up connection that backs up a lot, then
		// changes level
		if (host_client->message.overflowed)
		{
			SV_DropClient (true);
			host_client->message.overflowed = false;
			continue;
		}

		if (host_client->message.cursize || host_client->dropasap)
		{
			if (!NET_CanSendMessage (host_client->netconnection))
			{
//				I_Printf ("can't write\n");
				continue;
			}

			if (host_client->dropasap)
				SV_DropClient (false);	// went to another level
			else
			{
				if (NET_SendMessage (host_client->netconnection
				, &host_client->message) == -1)
					SV_DropClient (true);	// if the message couldn't send, kick off
				SZ_Clear (&host_client->message);
				host_client->last_message = realtime;
				if (host_client->sendsignon == PRESPAWN_FLUSH)
					host_client->sendsignon = PRESPAWN_DONE;
			}
		}
	}


// clear muzzle flashes
	SV_CleanupEnts ();
}


/*
==============================================================================

SERVER SPAWNING

==============================================================================
*/

#define SIGNON_SIZE		31500 // QS has a MAX_DATAGRAM of 32000, try to play nice

/*
================
SV_AddSignonBuffer
================
*/
static void SV_AddSignonBuffer (void)
{
	sizebuf_t *sb;
	if (sv.num_signon_buffers >= MAX_SIGNON_BUFFERS)
		Host_Error ("SV_AddSignonBuffer overflow\n");

	sb = (sizebuf_t *) Hunk_AllocName (sizeof (sizebuf_t) + SIGNON_SIZE, "signon");
	sb->data = (byte *)(sb + 1);
	sb->maxsize = SIGNON_SIZE;
	sv.signon_buffers[sv.num_signon_buffers++] = sb;
	sv.signon = sb;
}

/*
================
SV_ReserveSignonSpace
================
*/
void SV_ReserveSignonSpace (int numbytes)
{
	if (sv.signon->cursize + numbytes > sv.signon->maxsize)
		SV_AddSignonBuffer ();
}

/*
================
SV_ModelIndex

================
*/
int SV_ModelIndex (const char *name)
{
	int		i;

	if (!name || !name[0])
		return 0;

	for (i=0 ; i<MAX_MODELS && sv.model_precache[i] ; i++)
		if (!strcmp(sv.model_precache[i], name))
			return i;
	if (i==MAX_MODELS || !sv.model_precache[i])
		Sys_Error ("SV_ModelIndex: model %s not precached", name);
	return i;
}

/*
================
SV_CreateBaseline
================
*/
void SV_CreateBaseline (void)
{
	int			i;
	edict_t		*svent;
	int			entnum;
	int			bits; //johnfitz -- PROTOCOL_FITZQUAKE

	for (entnum = 0; entnum < qcvm->num_edicts ; entnum++)
	{
	// get the current server version
		svent = EDICT_NUM(entnum);
		if (svent->free)
			continue;
		if (entnum > svs.maxclients && !svent->v.modelindex)
			continue;

	//
	// create entity baseline
	//
		VectorCopy (svent->v.origin, svent->baseline.origin);
		VectorCopy (svent->v.angles, svent->baseline.angles);
		svent->baseline.frame = svent->v.frame;
		svent->baseline.skin = svent->v.skin;
		if (entnum > 0 && entnum <= svs.maxclients)
		{
			svent->baseline.colormap = entnum;
			svent->baseline.modelindex = SV_ModelIndex("progs/player.mdl");
			svent->baseline.alpha = ENTALPHA_DEFAULT; //johnfitz -- alpha support
			svent->baseline.scale = ENTSCALE_DEFAULT;
		}
		else
		{
			svent->baseline.colormap = 0;
			svent->baseline.modelindex = SV_ModelIndex(PR_GetString(svent->v.model));
			svent->baseline.alpha = svent->alpha; //johnfitz -- alpha support
			svent->baseline.scale = ENTSCALE_DEFAULT;
			if (sv.protocol == PROTOCOL_RMQ)
			{
				eval_t* val;
				val = GetEdictFieldValueByName(svent, "scale");
				if (val)
					svent->baseline.scale = ENTSCALE_ENCODE(val->_float);
			}
		}

		//johnfitz -- PROTOCOL_FITZQUAKE
		bits = 0;
		if (sv.protocol == PROTOCOL_NETQUAKE) //still want to send baseline in PROTOCOL_NETQUAKE, so reset these values
		{
			if (svent->baseline.modelindex & 0xFF00)
				svent->baseline.modelindex = 0;
			if (svent->baseline.frame & 0xFF00)
				svent->baseline.frame = 0;
			svent->baseline.alpha = ENTALPHA_DEFAULT;
			svent->baseline.scale = ENTSCALE_DEFAULT;
		}
		else //decide which extra data needs to be sent
		{
			if (svent->baseline.modelindex & 0xFF00)
				bits |= B_LARGEMODEL;
			if (svent->baseline.frame & 0xFF00)
				bits |= B_LARGEFRAME;
			if (svent->baseline.alpha != ENTALPHA_DEFAULT)
				bits |= B_ALPHA;
			if (svent->baseline.scale != ENTSCALE_DEFAULT)
				bits |= B_SCALE;
		}
		//johnfitz

	//
	// add to the message
	//
		SV_ReserveSignonSpace (35);

		//johnfitz -- PROTOCOL_FITZQUAKE
		if (bits)
			MSG_WriteByte (sv.signon, svc_spawnbaseline2);
		else
			MSG_WriteByte (sv.signon, svc_spawnbaseline);
		//johnfitz

		MSG_WriteShort (sv.signon,entnum);

		//johnfitz -- PROTOCOL_FITZQUAKE
		if (bits)
			MSG_WriteByte (sv.signon, bits);

		if (bits & B_LARGEMODEL)
			MSG_WriteShort (sv.signon, svent->baseline.modelindex);
		else
			MSG_WriteByte (sv.signon, svent->baseline.modelindex);

		if (bits & B_LARGEFRAME)
			MSG_WriteShort (sv.signon, svent->baseline.frame);
		else
			MSG_WriteByte (sv.signon, svent->baseline.frame);
		//johnfitz

		MSG_WriteByte (sv.signon, svent->baseline.colormap);
		MSG_WriteByte (sv.signon, svent->baseline.skin);
		for (i=0 ; i<3 ; i++)
		{
			MSG_WriteCoord(sv.signon, svent->baseline.origin[i], sv.protocolflags);
			MSG_WriteAngle(sv.signon, svent->baseline.angles[i], sv.protocolflags);
		}

		//johnfitz -- PROTOCOL_FITZQUAKE
		if (bits & B_ALPHA)
			MSG_WriteByte (sv.signon, svent->baseline.alpha);
		//johnfitz

		if (bits & B_SCALE)
			MSG_WriteByte (sv.signon, svent->baseline.scale);
	}
}


/*
================
SV_SendReconnect

Tell all the clients that the server is changing levels
================
*/
void SV_SendReconnect (void)
{
	byte	data[128];
	sizebuf_t	msg;

	msg.data = data;
	msg.cursize = 0;
	msg.maxsize = sizeof(data);

	MSG_WriteChar (&msg, svc_stufftext);
	MSG_WriteString (&msg, "reconnect\n");
	NET_SendToAll (&msg, 5.0);

	if (!isDedicated)
		Cmd_ExecuteString ("reconnect\n", src_command);
}


/*
================
SV_SaveSpawnparms

Grabs the current state of each client for saving across the
transition to another level
================
*/
void SV_SaveSpawnparms (void)
{
	int		i, j;

	svs.serverflags = pr_global_struct->serverflags;

	for (i=0, host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
	{
		if (!host_client->active)
			continue;

	// call the progs to get default spawn parms for the new client
		pr_global_struct->self = EDICT_TO_PROG(host_client->edict);
		PR_ExecuteProgram (pr_global_struct->SetChangeParms);
		for (j=0 ; j<NUM_SPAWN_PARMS ; j++)
			host_client->spawn_parms[j] = (&pr_global_struct->parm1)[j];
	}
}


/*
================
SV_SpawnServer

This is called at the start of each level
================
*/
extern float		scr_centertime_off;
void SV_SpawnServer (const char *server)
{
	static char	dummy[8] = { 0,0,0,0,0,0,0,0 };
	edict_t		*ent;
	int			i, signonsize;
	qcvm_t		*vm = qcvm;

	// let's not have any servers with no name
	if (hostname.string[0] == 0)
		Cvar_Set ("hostname", "UNNAMED");
	scr_centertime_off = 0;

	Con_DPrintf ("SpawnServer: %s\n",server);
	svs.changelevel_issued = false;		// now safe to issue another

	PR_SwitchQCVM(NULL);

//
// tell all connected clients that we are going to a new level
//
	if (sv.active)
	{
		SV_SendReconnect ();
	}

//
// make cvars consistant
//
	if (coop.value)
		Cvar_Set ("deathmatch", "0");
	current_skill = (int)(skill.value + 0.5);
	if (current_skill < 0)
		current_skill = 0;
	if (current_skill > 3)
		current_skill = 3;

	Cvar_SetValue ("skill", (float)current_skill);

//
// set up the new server
//
	//memset (&sv, 0, sizeof(sv));
	Host_ClearMemory ();

	q_strlcpy (sv.name, server, sizeof(sv.name));

	sv.protocol = sv_protocol; // johnfitz
	
	if (sv.protocol == PROTOCOL_RMQ)
	{
		// set up the protocol flags used by this server
		// (note - these could be cvar-ised so that server admins could choose the protocol features used by their servers)
		sv.protocolflags = PRFL_INT32COORD | PRFL_SHORTANGLE;
	}
	else sv.protocolflags = 0;

	PR_SwitchQCVM(vm);
// load progs to get entity field count
	PR_LoadProgs ("progs.dat", true);

// allocate server memory
	/* Host_ClearMemory() called above already cleared the whole sv structure */
	qcvm->max_edicts = CLAMP (MIN_EDICTS,(int)max_edicts.value,MAX_EDICTS); //johnfitz -- max_edicts cvar
	qcvm->edicts = (edict_t *) malloc (qcvm->max_edicts*qcvm->edict_size); // ericw -- sv.edicts switched to use malloc()
	ClearLink (&qcvm->free_edicts);

	sv.datagram.maxsize = sizeof(sv.datagram_buf);
	sv.datagram.cursize = 0;
	sv.datagram.data = sv.datagram_buf;

	sv.reliable_datagram.maxsize = sizeof(sv.reliable_datagram_buf);
	sv.reliable_datagram.cursize = 0;
	sv.reliable_datagram.data = sv.reliable_datagram_buf;

	SV_AddSignonBuffer ();

// leave slots at start for clients only
	qcvm->num_edicts = svs.maxclients+1;
	memset(qcvm->edicts, 0, qcvm->num_edicts*qcvm->edict_size); // ericw -- sv.edicts switched to use malloc()
	for (i=0 ; i<svs.maxclients ; i++)
	{
		ent = EDICT_NUM(i+1);
		svs.clients[i].edict = ent;
	}

	sv.state = ss_loading;
	sv.paused = false;
	sv.nomonsters = (nomonsters.value != 0.f);

	qcvm->time = 1.0;

	q_strlcpy (sv.name, server, sizeof(sv.name));
	q_snprintf (sv.modelname, sizeof(sv.modelname), "maps/%s.bsp", server);
	sv.worldmodel = Mod_ForName (sv.modelname, false);
	if (!sv.worldmodel)
	{
		Con_Printf ("Couldn't spawn server %s\n", sv.modelname);
		sv.active = false;
		return;
	}
	sv.models[1] = sv.worldmodel;

//
// clear world interaction links
//
	SV_ClearWorld ();

	sv.sound_precache[0] = dummy;
	sv.model_precache[0] = dummy;
	sv.model_precache[1] = sv.modelname;
	for (i=1 ; i<sv.worldmodel->numsubmodels ; i++)
	{
		sv.model_precache[1+i] = localmodels[i];
		sv.models[i+1] = Mod_ForName (localmodels[i], false);
	}

//
// load the rest of the entities
//
	ent = EDICT_NUM(0);
	memset (&ent->v, 0, qcvm->progs->entityfields * 4);
	ent->v.model = PR_SetEngineString(sv.worldmodel->name);
	ent->v.modelindex = 1;		// world model
	ent->v.solid = SOLID_BSP;
	ent->v.movetype = MOVETYPE_PUSH;

	if (coop.value)
		pr_global_struct->coop = coop.value;
	else
		pr_global_struct->deathmatch = deathmatch.value;

	pr_global_struct->mapname = PR_SetEngineString(sv.name);

// serverflags are for cross level information (sigils)
	pr_global_struct->serverflags = svs.serverflags;

	ED_LoadFromFile (sv.worldmodel->entities);

	sv.active = true;

// all setup is completed, any further precache statements are errors
	sv.state = ss_active;

// run two frames to allow everything to settle
	host_frametime = 0.1;
	SV_Physics ();
	SV_Physics ();

// create a baseline for more efficient communications
	SV_CreateBaseline ();

	//johnfitz -- warn if signon buffer larger than standard server can handle
	for (i = 0, signonsize = 0; i < sv.num_signon_buffers; i++)
		signonsize += sv.signon_buffers[i]->cursize;
	if (signonsize > 64000-2)
		Con_DWarning ("%i byte signon buffer exceeds QS limit of 63998.\n", signonsize);
	else if (signonsize > 8000-2) //max size that will fit into 8000-sized client->message buffer with 2 extra bytes on the end
		Con_DWarning ("%i byte signon buffer exceeds standard limit of 7998.\n", signonsize);
	//johnfitz

// send serverinfo to all connected clients
	for (i=0,host_client = svs.clients ; i<svs.maxclients ; i++, host_client++)
		if (host_client->active)
			SV_SendServerinfo (host_client);

	Con_DPrintf ("Server spawned.\n");
}

