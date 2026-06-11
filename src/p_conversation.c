/* p_conversation.c: parse Strife's binary conversation lumps.
 *
 * The on-disk layout (see the conversation node / choice tables) is a flat
 * sequence of fixed-size little-endian records.  A node is 1516 bytes:
 *
 *   0x000 speaker    (u32)         talker type id
 *   0x004 dropitem   (s32)         item dropped if killed, <0 none
 *   0x008 checkitem  (s32 * 3)     inventory-gated jump items
 *   0x014 link       (s32)         node to jump to if all checks pass
 *   0x018 name       (char * 16)
 *   0x028 voice      (char * 8)
 *   0x030 backpic    (char * 8)
 *   0x038 dialogue   (char * 320)
 *   0x178 choices    (228 * 5)
 *
 * and a choice is 228 bytes:
 *
 *   x+0x00 givetype   (s32)
 *   x+0x04 needitem   (s32 * 3)
 *   x+0x10 needamount (s32 * 3)
 *   x+0x1C text       (char * 32)
 *   x+0x3C yes        (char * 80)
 *   x+0x8C link       (s32)
 *   x+0x90 log        (u32)
 *   x+0x94 no         (char * 80)
 *
 * Strings are fixed-width and not necessarily NUL-terminated on disk, so each
 * is copied into a buffer one larger and terminated. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "lprintf.h"
#include "z_zone.h"
#include "d_event.h"
#include "d_player.h"
#include "p_mobj.h"
#include "doomstat.h"
#include "hu_lib.h"
#include "hu_stuff.h"
#include "v_video.h"
#include "p_conversation.h"

static conv_node_t *conv_nodes;
static int          conv_count;
static int          conv_cap;

static int conv_rd32(const byte *p)
{
  return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) |
               ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}

/* copy a fixed-width on-disk string into a NUL-terminated buffer */
static void conv_str(char *dst, const byte *src, int width)
{
  int i;
  for (i = 0; i < width; i++)
    dst[i] = (char)src[i];
  dst[width] = 0;
}

static void conv_parse_choice(conv_choice_t *c, const byte *p)
{
  int i;
  c->givetype = conv_rd32(p + 0x00);
  for (i = 0; i < 3; i++)
    c->needitem[i] = conv_rd32(p + 0x04 + 4 * i);
  for (i = 0; i < 3; i++)
    c->needamount[i] = conv_rd32(p + 0x10 + 4 * i);
  conv_str(c->text, p + 0x1C, CONV_CHOICE_TEXT_LEN);
  conv_str(c->yes,  p + 0x3C, CONV_CHOICE_MSG_LEN);
  c->link = conv_rd32(p + 0x8C);
  c->log  = conv_rd32(p + 0x90);
  conv_str(c->no,   p + 0x94, CONV_CHOICE_MSG_LEN);
}

static void conv_parse_node(conv_node_t *n, const byte *p)
{
  int i;
  n->speaker  = conv_rd32(p + 0x00);
  n->dropitem = conv_rd32(p + 0x04);
  for (i = 0; i < 3; i++)
    n->checkitem[i] = conv_rd32(p + 0x08 + 4 * i);
  n->link = conv_rd32(p + 0x14);
  conv_str(n->name,     p + 0x18, CONV_NAME_LEN);
  conv_str(n->voice,    p + 0x28, CONV_VOICE_LEN);
  conv_str(n->backpic,  p + 0x30, CONV_BACKPIC_LEN);
  conv_str(n->dialogue, p + 0x38, CONV_DIALOG_LEN);
  for (i = 0; i < CONV_NUM_CHOICES; i++)
    conv_parse_choice(&n->choices[i], p + 0x178 + CONV_CHOICE_SIZE * i);
}

void P_ConversationParse(const byte *data, int len)
{
  int count, i;
  if (!data || len < CONV_NODE_SIZE)
    return;
  count = len / CONV_NODE_SIZE;
  for (i = 0; i < count; i++)
  {
    const byte *rec = data + (size_t)i * CONV_NODE_SIZE;
    /* A speaker id of zero is not a real conversation actor; editors such as
     * Doom Builder routinely emit an all-zero DIALOGUE lump for UDMF maps that
     * have no dialogue, so skip those records rather than table them. */
    if (conv_rd32(rec) == 0)
      continue;
    if (conv_count >= conv_cap)
    {
      int ncap = conv_cap ? conv_cap * 2 : 16;
      conv_nodes = realloc(conv_nodes, (size_t)ncap * sizeof(*conv_nodes));
      conv_cap = ncap;
    }
    conv_parse_node(&conv_nodes[conv_count], rec);
    conv_count++;
  }
}

void P_ConversationClear(void)
{
  free(conv_nodes);
  conv_nodes = NULL;
  conv_count = 0;
  conv_cap   = 0;
}

int P_ConversationCount(void)
{
  return conv_count;
}

const conv_node_t *P_ConversationForSpeaker(int speaker)
{
  int i;
  /* The entry node for a speaker is its first node in parse order.  Links
   * index the parsed array as a whole (see P_ConversationNode), so the array
   * order must be preserved; when both SCRIPT00 and a map lump define the same
   * speaker the earlier-parsed entry wins, which matches the usual case of a
   * single active conversation lump. */
  for (i = 0; i < conv_count; i++)
    if (conv_nodes[i].speaker == speaker)
      return &conv_nodes[i];
  return NULL;
}

const conv_node_t *P_ConversationNode(int link)
{
  /* A link is the 1-based position of the target node in the parsed array
   * (the Nth conversation node defined); 0 means "no node" / end. */
  if (link <= 0 || link > conv_count)
    return NULL;
  return &conv_nodes[link - 1];
}

/* ------------------------------------------------------------------------- *
 * On-screen conversation runtime
 *
 * A conversation is modal-ish: while one is up it renders over the view and
 * the talker-player's use/fire buttons drive it, but it does not seize the
 * input system -- selection is edge-triggered off the player's command
 * buttons so ordinary play resumes untouched the moment it closes.  This layer
 * navigates the dialogue tree (text, choices, links) and does not yet apply a
 * choice's side effects.
 * ------------------------------------------------------------------------- */

extern patchnum_t hu_font[HU_FONTSIZE];

static int                conv_active;
static const conv_node_t *conv_cur;       /* current page                 */
static struct mobj_s     *conv_talker;    /* the other party (the user)   */
static int                conv_sel;        /* highlighted choice index     */
static int                conv_oldbtn;     /* previous tic's buttons       */

/* The choices actually offered: a choice with neither text nor link is unused
 * (the trailing "Bye" option is implicit).  Returns the count and fills idx[]
 * with the offered choice slots in order. */
static int conv_offered(const conv_node_t *n, int *idx)
{
  int i, c = 0;
  for (i = 0; i < CONV_NUM_CHOICES; i++)
    if (n->choices[i].text[0] || n->choices[i].link)
      idx[c++] = i;
  return c;
}

int P_ConversationStart(int speaker, struct mobj_s *talker)
{
  const conv_node_t *n = P_ConversationForSpeaker(speaker);
  if (!n)
    return 0;
  conv_active  = 1;
  conv_cur     = n;
  conv_talker  = talker;
  conv_sel     = 0;
  conv_oldbtn  = (talker && talker->player) ? talker->player->cmd.buttons : 0;
  return 1;
}

void P_ConversationEnd(void)
{
  conv_active  = 0;
  conv_cur     = NULL;
  conv_talker  = NULL;
}

int P_ConversationIsActive(void)
{
  return conv_active;
}

/* Move to the node a choice links to, or end the conversation. */
static void conv_follow(int link)
{
  const conv_node_t *n;
  if (link == 0)
  {
    P_ConversationEnd();
    return;
  }
  /* a link is the 1-based index of the target node in the parsed array */
  n = P_ConversationNode(link);
  if (n)
  {
    conv_cur = n;
    conv_sel = 0;
  }
  else
    P_ConversationEnd();                  /* dangling link: end gracefully */
}

/* Whether a top message is worth printing: Strife uses a lone "_" to mean
 * "no message", and an empty string is likewise nothing to show. */
static int conv_has_msg(const char *s)
{
  return s[0] && !(s[0] == '_' && s[1] == 0);
}

/* Apply a confirmed choice.  Without a Strife inventory a choice that demands
 * items or gold (a positive needitem/needamount) cannot be satisfied and takes
 * the failure path; a choice with no requirement succeeds.  The matching top
 * message is shown, and the conversation follows the choice's link (success)
 * or simply closes (failure with nowhere to go).  Item give/take and line
 * specials are a later step. */
static void conv_choose(const conv_choice_t *c)
{
  player_t *pl = (conv_talker && conv_talker->player) ? conv_talker->player
                                                      : NULL;
  int i, ok = 1;
  for (i = 0; i < 3; i++)
    if (c->needitem[i] > 0 || c->needamount[i] > 0)
      ok = 0;                            /* a requirement we cannot meet */

  if (pl)
  {
    const char *m = ok ? c->yes : c->no;
    if (conv_has_msg(m))
      pl->message = m;                   /* table storage outlives the line */                   /* table storage outlives the line */
  }

  if (ok)
    conv_follow(c->link);
  else
    P_ConversationEnd();
}

void P_ConversationTicker(void)
{
  player_t *pl;
  int btn, edge, idx[CONV_NUM_CHOICES], noff;
  if (!conv_active || !conv_cur)
    return;
  /* abandon if the talker is gone */
  if (!conv_talker || !conv_talker->player)
  {
    P_ConversationEnd();
    return;
  }
  pl   = conv_talker->player;
  btn  = pl->cmd.buttons;
  edge = btn & ~conv_oldbtn;            /* buttons pressed this tic */
  conv_oldbtn = btn;

  noff = conv_offered(conv_cur, idx);

  if (edge & BT_ATTACK)                 /* cycle the highlighted choice */
    conv_sel = (conv_sel + 1) % (noff + 1);   /* +1 for the implicit Bye */

  if (edge & BT_USE)                    /* confirm the highlighted choice */
  {
    if (noff > 0 && conv_sel < noff)
      conv_choose(&conv_cur->choices[idx[conv_sel]]);
    else
      P_ConversationEnd();              /* the implicit Bye option */
  }
}

/* draw one line of small-font text at a virtual 320x200 position */
static void conv_text(int x, int y, int cm, const char *s)
{
  hu_textline_t l;
  HUlib_initTextLine(&l, x, y, hu_font, HU_FONTSTART, cm);
  for (; *s; s++)
    HUlib_addCharToTextLine(&l, *s);
  HUlib_drawTextLine(&l, false);
}

void P_ConversationDrawer(void)
{
  const conv_node_t *n = conv_cur;
  int idx[CONV_NUM_CHOICES], noff, i, y;
  const char *name;
  if (!conv_active || !n)
    return;
  name = n->name[0] ? n->name : "Person";
  conv_text(8, 8, CR_GOLD, name);
  conv_text(8, 24, CR_GRAY, n->dialogue);

  noff = conv_offered(n, idx);
  y = 130;
  for (i = 0; i < noff; i++)
  {
    char line[CONV_CHOICE_TEXT_LEN + 24];
    const conv_choice_t *c = &n->choices[idx[i]];
    /* a positive first need-amount is a gold price; Strife shows it as a
     * " for <count>" suffix on the reply text (this is how shop lines read) */
    if (c->needamount[0] > 0)
      snprintf(line, sizeof(line), "%c %.*s for %d",
               (i == conv_sel) ? '>' : ' ', CONV_CHOICE_TEXT_LEN, c->text,
               c->needamount[0]);
    else
      snprintf(line, sizeof(line), "%c %.*s",
               (i == conv_sel) ? '>' : ' ', CONV_CHOICE_TEXT_LEN, c->text);
    conv_text(16, y, (i == conv_sel) ? CR_GREEN : CR_GRAY, line);
    y += 10;
  }
  /* implicit dismissal option */
  {
    char line[16];
    snprintf(line, sizeof(line), "%c BYE", (conv_sel == noff) ? '>' : ' ');
    conv_text(16, y, (conv_sel == noff) ? CR_GREEN : CR_GRAY, line);
  }
}
