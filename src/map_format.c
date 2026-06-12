/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *  Map format abstraction (reduced) - implementation.
 *
 *  See map_format.h.  The Doom descriptor points every dispatcher at the
 *  engine's existing functions, so dispatching through map_format is
 *  behaviourally identical to calling them directly.
 *
 *-----------------------------------------------------------------------------
 */

#include "doomstat.h"
#include "p_spec.h"
#include "map_format.h"
#include "hexen/p_spec_hexen.h"
#include "p_zacs.h"
#include "g_game.h"
#include "s_sound.h"
#include "sounds.h"
#include "d_deh.h"
#include "lprintf.h"

map_format_t map_format;

static const map_format_t doom_map_format =
{
  false,                      /* zdoom    */
  false,                      /* hexen    */
  false,                      /* polyobjs */
  false,                      /* acs      */
  false,                      /* sndseq   */
  false,                      /* animdefs */
  false,                      /* doublesky*/
  P_CrossSpecialLine,
  P_ShootSpecialLine,
  P_PlayerInSpecialSector,
  NULL,                       /* execute_line_special (Doom: none) */
  VF_DOOM
};

/* Hexen map format.  The format flags drive the Hexen-sized linedef/thing
 * parsing in p_setup; polyobjs/acs/sndseq/animdefs are declared here but
 * their handlers are added in later commits.  The per-line trigger
 * dispatchers still point at the Doom functions for now -- Hexen's scripted
 * line specials land with the specials layer -- so a Hexen map currently
 * loads and renders with Doom-style line activation. */
static const map_format_t hexen_map_format =
{
  false,                      /* zdoom    */
  true,                       /* hexen    */
  true,                       /* polyobjs */
  true,                       /* acs      */
  true,                       /* sndseq   */
  true,                       /* animdefs */
  false,                      /* doublesky*/
  P_CrossHexenSpecialLine,
  P_ShootHexenSpecialLine,
  P_PlayerInSpecialSector,
  P_ExecuteHexenLineSpecial,
  VF_HEXEN
};

void P_ApplyMapFormat(void)
{
  if (hexen)
    map_format = hexen_map_format;
  else
    map_format = doom_map_format;
}

/* ZDoom 'Doom-in-Hexen': Hexen-sized THINGS/LINEDEFS records in a Doom-game
 * map (detected by a BEHAVIOR lump).  The hexen format flag drives the
 * Hexen-stride parsing in p_setup and the positive-bit thing filtering in
 * P_SpawnMapThing; zdoom marks the specials as ZDoom-numbered.
 *
 * ZDoom action specials share Hexen's special+args encoding and -- for the
 * classic range -- its numbering, so activation routes through the Hexen
 * dispatchers and the executor below handles the ZDoom-only cases before
 * delegating the rest to the Hexen executor (whose movers are game-aware). */

/* ZDoom lock numbers for Doom-format games follow the Boom generalized
 * scheme plus combination locks: 1-3 cards, 4-6 skulls, 100 any key,
 * 101 all keys, 129-131 card-or-skull of each colour. */
static dbool P_CheckZDoomLock(mobj_t *mo, int lock)
{
  player_t   *p;
  const char *msg = NULL;
  dbool       ok  = false;

  if (!mo || !mo->player)
    return false;
  p = mo->player;

  switch (lock)
  {
    case 0:
      return true;
    case 1:   ok = p->cards[it_redcard];    msg = s_PD_REDK;    break;
    case 2:   ok = p->cards[it_bluecard];   msg = s_PD_BLUEK;   break;
    case 3:   ok = p->cards[it_yellowcard]; msg = s_PD_YELLOWK; break;
    case 4:   ok = p->cards[it_redskull];   msg = s_PD_REDK;    break;
    case 5:   ok = p->cards[it_blueskull];  msg = s_PD_BLUEK;   break;
    case 6:   ok = p->cards[it_yellowskull];msg = s_PD_YELLOWK; break;
    case 100:
      ok = p->cards[it_redcard] || p->cards[it_bluecard] ||
           p->cards[it_yellowcard] || p->cards[it_redskull] ||
           p->cards[it_blueskull] || p->cards[it_yellowskull];
      msg = s_PD_ANY;
      break;
    case 101:
      ok = p->cards[it_redcard] && p->cards[it_bluecard] &&
           p->cards[it_yellowcard] && p->cards[it_redskull] &&
           p->cards[it_blueskull] && p->cards[it_yellowskull];
      msg = s_PD_ALL6;
      break;
    case 129: ok = p->cards[it_redcard]    || p->cards[it_redskull];
              msg = s_PD_REDK;    break;
    case 130: ok = p->cards[it_bluecard]   || p->cards[it_blueskull];
              msg = s_PD_BLUEK;   break;
    case 131: ok = p->cards[it_yellowcard] || p->cards[it_yellowskull];
              msg = s_PD_YELLOWK; break;
    default:
      lprintf(LO_WARN, "P_CheckZDoomLock: unhandled lock %d, opening\n", lock);
      return true;
  }

  if (!ok)
  {
    p->message = msg;
    S_StartSound(mo, sfx_oof);
  }
  return ok;
}

static dbool P_ExecuteZDoomLineSpecial(int special, int *args, line_t *line,
                                       int side, mobj_t *mo)
{
  switch (special)
  {
    case 13:                    /* Door_LockedRaise: Doom keys, not Hexen's */
      if (!P_CheckZDoomLock(mo, args[3]))
        return false;
      return P_ExecuteHexenLineSpecial(12, args, line, side, mo);
    case 37:                    /* Floor_MoveToValue */
      return Hexen_EV_DoFloor(line, args, FLEV_MOVETOVALUE);
    case 193:                   /* Ceiling_LowerInstant */
      return Hexen_EV_DoCeiling(line, args, CLEV_LOWERTIMES8INSTANT);
    case 194:                   /* Ceiling_RaiseInstant */
      return Hexen_EV_DoCeiling(line, args, CLEV_RAISETIMES8INSTANT);
    case 215:                   /* Teleport_Line(thisid, destid, flip) */
    {
      /* EV_SilentLineTeleport pairs lines through the source line's tag;
       * ZDoom names the destination in args[1].  Swap the tag for the
       * lookup and restore it -- the teleport itself does not re-read it. */
      int saved, ok;
      if (!line)
        return false;
      saved = line->tag;
      line->tag = args[1];
      ok = EV_SilentLineTeleport(line, side, mo, args[2] != 0);
      line->tag = saved;
      return ok;
    }
    case 80:                    /* ACS_Execute */
      return Z_ACSStart(args[0], args[1], &args[2], 3, mo, line, side, false);
    case 226:                   /* ACS_ExecuteAlways */
      return Z_ACSStart(args[0], args[1], &args[2], 3, mo, line, side, true);
    case 274:                   /* ACS_NamedExecute */
      return Z_ACSStartNamed(args[0], args[1], &args[2], 3, mo, line, side, false);
    case 280:                   /* ACS_NamedExecuteAlways */
      return Z_ACSStartNamed(args[0], args[1], &args[2], 3, mo, line, side, true);
    case 279:                   /* ACS_NamedExecuteWithResult */
      return Z_ACSStartNamed(args[0], 0, &args[1], 3, mo, line, side, true);
    case 81:                    /* ACS_Suspend */
      return Z_ACSSuspend(args[0]);
    case 82:                    /* ACS_Terminate */
      return Z_ACSTerminate(args[0]);
    case 83:                    /* ACS_LockedExecute */
      if (!P_CheckZDoomLock(mo, args[4]))
        return false;
      return Z_ACSStart(args[0], args[1], &args[2], 2, mo, line, side, false);
    case 243:                   /* Exit_Normal */
      G_ExitLevel();
      return true;
    case 244:                   /* Exit_Secret */
      G_SecretExitLevel();
      return true;
    case 206:                   /* Plat_DownWaitUpStayLip */
      return EV_DoHexenPlat(line, args, PLAT_DOWNWAITUPSTAY, args[3]);
    case 207:                   /* Plat_PerpetualRaiseLip */
      return EV_DoHexenPlat(line, args, PLAT_PERPETUALRAISE, args[3]);
    case 181:                   /* Plane_Align (slopes): unsupported */
    case 208:                   /* TranslucentLine: static, applied at load */
      return false;
    case 132:                   /* ChangeCamera(tid, who, revert) */
    {
      /* Point the view at the actor named by args[0]'s tid; tid 0 reverts to
       * the player's own body.  Single-player only follows one view, so the
       * "who" selector is ignored.  ZDoom treats this as a smooth view hand
       * over; here it is an immediate switch the renderer reads each frame. */
      if (args[0] == 0)
        zacs_view_camera = NULL;
      else
      {
        int sp = -1;
        mobj_t *cam = P_FindMobjFromTID((short)args[0], &sp);
        zacs_view_camera = cam;       /* NULL if no such tid: stays on player */
      }
      return true;
    }
    default:
      return P_ExecuteHexenLineSpecial(special, args, line, side, mo);
  }
}

static const map_format_t zdoom_in_doom_map_format =
{
  true,                       /* zdoom    */
  true,                       /* hexen    */
  true,                       /* polyobjs */
  true,                       /* acs (ZACS VM; active when BEHAVIOR loads) */
  false,                      /* sndseq   */
  false,                      /* animdefs */
  false,                      /* doublesky*/
  P_CrossHexenSpecialLine,
  P_ShootHexenSpecialLine,
  P_PlayerInSpecialSector,    /* sector specials are translated to Doom's */
  P_ExecuteZDoomLineSpecial,
  VF_DOOM
};

void P_ApplyZDoomInDoomMapFormat(void)
{
  map_format = zdoom_in_doom_map_format;
}
