/* Hexen sound sequences (sn_sonix) -- runtime.
 *
 * This file holds the sequence runtime: the active-sequence list and the
 * per-tic stepper that walks each sequence's compiled command stream.  The
 * SNDSEQ lump parser (SN_InitSequenceScript) is added in a following commit;
 * until then SequenceData[] is empty and starting a sequence is a safe no-op
 * (the movers keep their engine-default sounds). */

#include <string.h>

#include "doomtype.h"
#include "doomdef.h"
#include "z_zone.h"
#include "m_fixed.h"
#include "m_random.h"
#include "s_sound.h"
#include "sounds.h"
#include "i_system.h"
#include "lprintf.h"
#include "w_wad.h"
#include "u_scanner.h"
#include "dsda_hacked.h"
#include "hexen/sn_sonix.h"

#define SS_MAX_SCRIPTS          64
#define SS_SEQUENCE_NAME_LENGTH 32
#define SS_TEMPBUFFER_SIZE      1024
#define SS_SCRIPT_NAME          "SNDSEQ"

/* Compiled sequence opcodes. */
enum
{
  SS_CMD_NONE,
  SS_CMD_PLAY,
  SS_CMD_WAITUNTILDONE,     /* used by PLAYUNTILDONE */
  SS_CMD_PLAYTIME,
  SS_CMD_PLAYREPEAT,
  SS_CMD_DELAY,
  SS_CMD_DELAYRAND,
  SS_CMD_VOLUME,
  SS_CMD_STOPSOUND,
  SS_CMD_END
};

struct seq_translate_s
{
  char name[SS_SEQUENCE_NAME_LENGTH];
  int  scriptNum;
  int  stopSound;
};

/* Maps each SEQ_* to the named script it plays and its stop sound.  Several
 * platform/door variants share a script (a 'heavy' or 'creak' platform is
 * just a platform). */
struct seq_translate_s SequenceTranslate[SEQ_NUMSEQ] =
{
  { "Platform",       0, 0 },
  { "Platform",       0, 0 },   /* heavy  */
  { "PlatformMetal",  0, 0 },
  { "Platform",       0, 0 },   /* creak  */
  { "Silence",        0, 0 },
  { "Lava",           0, 0 },
  { "Water",          0, 0 },
  { "Ice",            0, 0 },
  { "Earth",          0, 0 },
  { "PlatformMetal2", 0, 0 },
  { "DoorNormal",     0, 0 },
  { "DoorHeavy",      0, 0 },
  { "DoorMetal",      0, 0 },
  { "DoorCreak",      0, 0 },
  { "Silence",        0, 0 },
  { "Lava",           0, 0 },
  { "Water",          0, 0 },
  { "Ice",            0, 0 },
  { "Earth",          0, 0 },
  { "DoorMetal2",     0, 0 },
  { "Wind",           0, 0 }
};

int  *SequenceData[SS_MAX_SCRIPTS];

int        ActiveSequences;
seqnode_t *SequenceListHead;

/* Resolve a sound name (as written in the SNDSEQ lump) to its sfx index. */
static int GetSoundOffset(const char *name)
{
  int i;

  for (i = 1; i < num_sfx; i++)
    if (S_sfx[i].name && !strcasecmp(name, S_sfx[i].name))
      return i;
  return 0;
}

void SN_InitSequenceScript(void)
{
  int           i;
  int           lump;
  int           length;
  const char   *data;
  u_scanner_t   s;
  int           inSequence = -1;
  int          *tempDataStart = NULL;
  int          *tempDataPtr = NULL;
  int           slot = 0;

  ActiveSequences = 0;
  for (i = 0; i < SS_MAX_SCRIPTS; i++)
    SequenceData[i] = NULL;

  lump = W_CheckNumForName(SS_SCRIPT_NAME);
  if (lump < 0)
    return;
  length = W_LumpLength(lump);
  data   = (const char *) W_CacheLumpNum(lump);

  s = U_ScanOpen(data, length, SS_SCRIPT_NAME);

  while (U_GetNextToken(&s, true))
  {
    if (s.token == ':')
    {
      /* Begin a new named sequence. */
      if (inSequence != -1)
        U_Error(&s, "SN_InitSequenceScript: nested sequence");
      if (!U_GetNextToken(&s, true))
        break;

      tempDataStart = (int *) Z_Malloc(SS_TEMPBUFFER_SIZE * sizeof(int),
                                       PU_STATIC, 0);
      memset(tempDataStart, 0, SS_TEMPBUFFER_SIZE * sizeof(int));
      tempDataPtr = tempDataStart;

      for (slot = 0; slot < SS_MAX_SCRIPTS; slot++)
        if (SequenceData[slot] == NULL)
          break;
      if (slot == SS_MAX_SCRIPTS)
        I_Error("SN_InitSequenceScript: too many sequences");

      for (i = 0; i < SEQ_NUMSEQ; i++)
        if (!strcasecmp(SequenceTranslate[i].name, s.string))
        {
          SequenceTranslate[i].scriptNum = slot;
          inSequence = i;
          break;
        }
      /* A sequence not named in SequenceTranslate is parsed and discarded. */
      continue;
    }

    if (inSequence == -1)
      continue;

    if (!strcasecmp(s.string, "playuntildone"))
    {
      U_GetNextToken(&s, true);
      *tempDataPtr++ = SS_CMD_PLAY;
      *tempDataPtr++ = GetSoundOffset(s.string);
      *tempDataPtr++ = SS_CMD_WAITUNTILDONE;
    }
    else if (!strcasecmp(s.string, "play"))
    {
      U_GetNextToken(&s, true);
      *tempDataPtr++ = SS_CMD_PLAY;
      *tempDataPtr++ = GetSoundOffset(s.string);
    }
    else if (!strcasecmp(s.string, "playtime"))
    {
      U_GetNextToken(&s, true);
      *tempDataPtr++ = SS_CMD_PLAY;
      *tempDataPtr++ = GetSoundOffset(s.string);
      *tempDataPtr++ = SS_CMD_DELAY;
      *tempDataPtr++ = U_MustGetInteger(&s);
    }
    else if (!strcasecmp(s.string, "playrepeat"))
    {
      U_GetNextToken(&s, true);
      *tempDataPtr++ = SS_CMD_PLAYREPEAT;
      *tempDataPtr++ = GetSoundOffset(s.string);
    }
    else if (!strcasecmp(s.string, "delay"))
    {
      *tempDataPtr++ = SS_CMD_DELAY;
      *tempDataPtr++ = U_MustGetInteger(&s);
    }
    else if (!strcasecmp(s.string, "delayrand"))
    {
      *tempDataPtr++ = SS_CMD_DELAYRAND;
      *tempDataPtr++ = U_MustGetInteger(&s);
      *tempDataPtr++ = U_MustGetInteger(&s);
    }
    else if (!strcasecmp(s.string, "volume"))
    {
      *tempDataPtr++ = SS_CMD_VOLUME;
      *tempDataPtr++ = U_MustGetInteger(&s);
    }
    else if (!strcasecmp(s.string, "stopsound"))
    {
      U_GetNextToken(&s, true);
      SequenceTranslate[inSequence].stopSound = GetSoundOffset(s.string);
      *tempDataPtr++ = SS_CMD_STOPSOUND;
    }
    else if (!strcasecmp(s.string, "end"))
    {
      int   dataSize;
      *tempDataPtr++ = SS_CMD_END;
      dataSize = (int)((tempDataPtr - tempDataStart) * sizeof(int));
      SequenceData[slot] = (int *) Z_Malloc(dataSize, PU_STATIC, 0);
      memcpy(SequenceData[slot], tempDataStart, dataSize);
      Z_Free(tempDataStart);
      tempDataStart = NULL;
      inSequence = -1;
    }
    else
      U_Error(&s, "SN_InitSequenceScript: unknown command");
  }

  if (tempDataStart)
    Z_Free(tempDataStart);
  U_ScanClose(&s);
}

void SN_StartSequence(mobj_t *mobj, int sequence)
{
  seqnode_t *node;

  if (sequence < 0 || sequence >= SEQ_NUMSEQ)
    return;

  SN_StopSequence(mobj);        /* stop any previous sequence on this origin */

  /* Until the parser populates SequenceData[], there is nothing to play. */
  if (!SequenceData[SequenceTranslate[sequence].scriptNum])
    return;

  node = Z_Malloc(sizeof(*node), PU_STATIC, 0);
  memset(node, 0, sizeof(*node));
  node->sequencePtr = SequenceData[SequenceTranslate[sequence].scriptNum];
  node->sequence = sequence;
  node->mobj = mobj;
  node->delayTics = 0;
  node->stopSound = SequenceTranslate[sequence].stopSound;
  node->volume = 127;           /* start at max volume */

  if (!SequenceListHead)
  {
    SequenceListHead = node;
    node->next = node->prev = NULL;
  }
  else
  {
    SequenceListHead->prev = node;
    node->next = SequenceListHead;
    node->prev = NULL;
    SequenceListHead = node;
  }
  ActiveSequences++;
}

void SN_StartSequenceName(mobj_t *mobj, const char *name)
{
  int i;

  for (i = 0; i < SEQ_NUMSEQ; i++)
    if (!strcmp(name, SequenceTranslate[i].name))
    {
      SN_StartSequence(mobj, i);
      return;
    }
}

void SN_StopSequence(mobj_t *mobj)
{
  seqnode_t *node;
  seqnode_t *next_node;

  for (node = SequenceListHead; node; node = next_node)
  {
    next_node = node->next;
    if (node->mobj == mobj)
    {
      S_StopSound(mobj);
      if (node->stopSound)
        S_StartAmbientSound(mobj, node->stopSound, node->volume);
      if (SequenceListHead == node)
        SequenceListHead = node->next;
      if (node->prev)
        node->prev->next = node->next;
      if (node->next)
        node->next->prev = node->prev;
      Z_Free(node);
      ActiveSequences--;
    }
  }
}

void SN_UpdateActiveSequences(void)
{
  seqnode_t *node;
  seqnode_t *next_node;
  dbool      sndPlaying;

  if (!ActiveSequences)
    return;

  for (node = SequenceListHead; node; node = next_node)
  {
    next_node = node->next;
    if (node->delayTics)
    {
      node->delayTics--;
      continue;
    }
    sndPlaying = S_GetSoundPlayingInfo(node->mobj, node->currentSoundID);
    switch (*node->sequencePtr)
    {
      case SS_CMD_PLAY:
        if (!sndPlaying)
        {
          node->currentSoundID = *(node->sequencePtr + 1);
          S_StartAmbientSound(node->mobj, node->currentSoundID, node->volume);
        }
        node->sequencePtr += 2;
        break;
      case SS_CMD_WAITUNTILDONE:
        if (!sndPlaying)
        {
          node->sequencePtr++;
          node->currentSoundID = 0;
        }
        break;
      case SS_CMD_PLAYREPEAT:
        if (!sndPlaying)
        {
          node->currentSoundID = *(node->sequencePtr + 1);
          S_StartAmbientSound(node->mobj, node->currentSoundID, node->volume);
        }
        break;
      case SS_CMD_DELAY:
        node->delayTics = *(node->sequencePtr + 1);
        node->sequencePtr += 2;
        node->currentSoundID = 0;
        break;
      case SS_CMD_DELAYRAND:
      {
        int lo = *(node->sequencePtr + 1);
        int hi = *(node->sequencePtr + 2);
        int span = hi - lo;
        node->delayTics = lo + (span > 0 ? (M_Random() % span) : 0);
        node->sequencePtr += 3;
        node->currentSoundID = 0;
        break;
      }
      case SS_CMD_VOLUME:
        node->volume = (127 * (*(node->sequencePtr + 1))) / 100;
        node->sequencePtr += 2;
        break;
      case SS_CMD_STOPSOUND:
        /* Wait until something else stops the sequence. */
        break;
      case SS_CMD_END:
        SN_StopSequence(node->mobj);
        break;
      default:
        break;
    }
  }
}

/* Savegame support: where in its script a running sequence is, and pushing
 * that position back into a node restarted on load. */

int SN_GetSequenceOffset(int sequence, int *sequencePtr)
{
  return (sequencePtr - SequenceData[SequenceTranslate[sequence].scriptNum]);
}

void SN_ChangeNodeData(int nodeNum, int seqOffset, int delayTics, int volume,
                       int currentSoundID)
{
  int i;
  seqnode_t *node;

  i = 0;
  node = SequenceListHead;
  while (node && i < nodeNum)
  {
    node = node->next;
    i++;
  }
  if (!node)
  {              /* reached the end of the list before the nodeNum-th node */
    return;
  }
  node->delayTics = delayTics;
  node->volume = volume;
  node->sequencePtr += seqOffset;
  node->currentSoundID = currentSoundID;
}

void SN_StopAllSequences(void)
{
  seqnode_t *node;
  seqnode_t *next_node;

  for (node = SequenceListHead; node; node = next_node)
  {
    next_node = node->next;
    node->stopSound = 0;        /* don't play any stop sounds */
    SN_StopSequence(node->mobj);
  }
}
