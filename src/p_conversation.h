/* p_conversation.h: Strife conversation (dialogue) data.
 *
 * Strife stores per-NPC conversations in binary lumps -- SCRIPTxy for the
 * matching MAPxy slot, SCRIPT00 for conversations valid on every map, and a
 * DIALOGUE lump inside a UDMF map's directory.  Each conversation node is a
 * fixed 1516-byte record keyed by the speaker's type id (the object's
 * conversation id / DeHackEd number), holding the spoken text, an optional
 * name/voice/backdrop, item-gated jumps, and up to five player choices.
 *
 * This module only parses that binary form into an in-memory table keyed by
 * speaker id; it does not by itself drive any on-screen conversation.  When no
 * conversation lump is present (ordinary Doom/Heretic/Hexen content) the table
 * stays empty and every lookup reports "absent". */

#ifndef P_CONVERSATION_H
#define P_CONVERSATION_H

#include "doomtype.h"

#define CONV_NAME_LEN     16
#define CONV_VOICE_LEN     8
#define CONV_BACKPIC_LEN   8
#define CONV_DIALOG_LEN  320
#define CONV_CHOICE_TEXT_LEN  32
#define CONV_CHOICE_MSG_LEN   80
#define CONV_NUM_CHOICES       5
#define CONV_NODE_SIZE      1516   /* on-disk bytes per conversation node */
#define CONV_CHOICE_SIZE     228   /* on-disk bytes per choice            */

typedef struct {
  int  givetype;                       /* item id given on success (<0 none) */
  int  needitem[3];                    /* item ids required (<0 none)        */
  int  needamount[3];                  /* minimum amounts (slot 0 = gold)    */
  char text[CONV_CHOICE_TEXT_LEN + 1]; /* what the player says               */
  char yes[CONV_CHOICE_MSG_LEN + 1];   /* top message on success             */
  char no[CONV_CHOICE_MSG_LEN + 1];    /* top message on failure             */
  int  link;                           /* node this reply leads to (0 = end) */
  int  log;                            /* LOG<number> entry id on success    */
} conv_choice_t;

typedef struct {
  int  speaker;                        /* talker type id (conversation id)   */
  int  dropitem;                       /* item dropped if killed (<0 none)   */
  int  checkitem[3];                   /* inventory-gated jump items         */
  int  link;                           /* node to jump to if checks pass     */
  char name[CONV_NAME_LEN + 1];        /* displayed talker name              */
  char voice[CONV_VOICE_LEN + 1];      /* voice sound lump                   */
  char backpic[CONV_BACKPIC_LEN + 1];  /* backdrop picture lump              */
  char dialogue[CONV_DIALOG_LEN + 1];  /* what the talker says               */
  conv_choice_t choices[CONV_NUM_CHOICES];
} conv_node_t;

/* Parse a binary conversation lump (CONV_NODE_SIZE-byte records) into the
 * conversation table.  May be called more than once (e.g. SCRIPT00 then the
 * map's own lump); later nodes for a speaker override earlier ones.  Does
 * nothing for a NULL/short buffer. */
void P_ConversationParse(const byte *data, int len);

/* Drop the whole conversation table (level teardown / new game). */
void P_ConversationClear(void);

/* Number of parsed conversation nodes (0 when no lump was present). */
int P_ConversationCount(void);

/* The conversation node for a speaker type id, or NULL if none.  The first
 * node parsed for a given speaker wins ties only after explicit override
 * rules; here the most recently parsed node for a speaker is returned. */
const conv_node_t *P_ConversationForSpeaker(int speaker);

/* ------------------------------------------------------------------------- *
 * On-screen conversation runtime.
 *
 * P_ConversationStart opens the conversation for a speaker type id, with the
 * given talker actor as the other party, and returns 1 if one exists (and is
 * now on screen) or 0 if not.  While a conversation is active the ticker reads
 * the talker-player's buttons to move the highlighted choice and to confirm
 * it, and the drawer paints the speaker name, the page text and the numbered
 * choices.  Confirming a choice follows its link to the next node or ends the
 * conversation; this layer renders and navigates the dialogue tree but does
 * not yet apply choice side effects (item give/take, line specials).
 * ------------------------------------------------------------------------- */
int  P_ConversationStart(int speaker, struct mobj_s *talker);
void P_ConversationTicker(void);
void P_ConversationDrawer(void);
void P_ConversationEnd(void);
int  P_ConversationIsActive(void);

#endif /* P_CONVERSATION_H */
