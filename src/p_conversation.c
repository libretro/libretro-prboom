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
#include <string.h>

#include "doomtype.h"
#include "lprintf.h"
#include "z_zone.h"
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
    if (conv_count >= conv_cap)
    {
      int ncap = conv_cap ? conv_cap * 2 : 16;
      conv_nodes = realloc(conv_nodes, (size_t)ncap * sizeof(*conv_nodes));
      conv_cap = ncap;
    }
    conv_parse_node(&conv_nodes[conv_count], data + (size_t)i * CONV_NODE_SIZE);
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
  /* later definitions override earlier ones, so search from the end */
  for (i = conv_count - 1; i >= 0; i--)
    if (conv_nodes[i].speaker == speaker)
      return &conv_nodes[i];
  return NULL;
}
