/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_CHATLOG_H
#define KP_CHATLOG_H

#include <gio/gio.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A recorded YouTube live chat (`Title [ID].live_chat.json` from yt-dlp,
 * one JSON object per line) read by time. The parsing of the renderers
 * (text, Super Chat, members, channel emoji in runs) follows kikarinhas'
 * youtube.c, which reads the same shapes live.
 *
 * The file can pass 100 MB, so nothing is decoded up front: opening maps
 * the file and one pass over it keeps, for each line that holds a message,
 * its time and place (16 bytes each, a few MB for a long live). A line is
 * only parsed when its time comes (kp_chatlog_decode). Lines without
 * messages (tickers, placeholders, deletions) are left out of the index.
 *
 * Memory: kp_chatlog owns the mapping and the index; a kp_chat_item owns
 * its strings (kp_chat_item_free). */

typedef enum {
    KP_CHAT_TEXT,
    KP_CHAT_PAID,   /* Super Chat / Super Sticker */
    KP_CHAT_MEMBER, /* new member or milestone */
} kp_chat_kind;

enum {
    KP_CHAT_OWNER = 1 << 0,
    KP_CHAT_MOD = 1 << 1,
    KP_CHAT_MEMBERBADGE = 1 << 2,
    KP_CHAT_VERIFIED = 1 << 3,
};

/* A channel emoji: an image the text names (":_goku:") at text[start,
 * start+len). */
typedef struct {
    char *id, *url;
    size_t start, len;
} kp_chat_emote;

typedef struct {
    int64_t t_ms; /* where in the video it appears */
    kp_chat_kind kind;
    unsigned badges;
    char *user_id, *name; /* name without the "@" of handles */
    char *avatar;         /* picture URL or NULL */
    char *text;           /* may be empty */
    char *amount;         /* PAID: "R$ 10,00"; else NULL */
    kp_chat_emote *emotes;
    int n_emotes;
} kp_chat_item;

void kp_chat_item_free(kp_chat_item *it);

typedef struct kp_chatlog kp_chatlog;

/* Maps and indexes path (blocking: for tests and small files); NULL with
 * *err set (g_free it). */
kp_chatlog *kp_chatlog_open(const char *path, char **err);
/* The same in a thread; done(log, err, ud) runs on the main loop with the
 * log, or NULL and an error text (valid during the call). If cancellable
 * is cancelled, done still runs (with NULL). */
typedef void (*kp_chatlog_done)(kp_chatlog *log, const char *err, void *ud);
void kp_chatlog_open_async(const char *path, GCancellable *cancellable, kp_chatlog_done done,
                           void *ud);
void kp_chatlog_free(kp_chatlog *log);

/* Lines that hold messages, in time order. */
size_t kp_chatlog_count(const kp_chatlog *log);
int64_t kp_chatlog_time(const kp_chatlog *log, size_t i);
/* Index of the first line after t_ms (count if none). */
size_t kp_chatlog_after(const kp_chatlog *log, int64_t t_ms);
/* Parses line i and appends its messages (kp_chat_item *, free each with
 * kp_chat_item_free) to out. Returns how many. */
int kp_chatlog_decode(const kp_chatlog *log, size_t i, GPtrArray *out);

/* Whether a message passes the "only" filter: 0 all, 1 Super Chats and
 * members, 2 owner, moderators and members. */
bool kp_chat_item_shown(const kp_chat_item *it, int filter);

#endif
