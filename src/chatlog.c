/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "chatlog.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

#define MAX_TEXT 400
#define MAX_EMOTES 32
/* Channel emoji come as 24/48 px thumbnails; this is what is asked for. */
#define EMOTE_PX 48

typedef struct {
    int64_t t_ms;
    uint64_t off;
    uint32_t len;
} entry;

struct kp_chatlog {
    GMappedFile *map;
    const char *data;
    size_t size;
    entry *index;
    size_t n, cap;
};

void kp_chat_item_free(kp_chat_item *it)
{
    if (!it)
        return;
    g_free(it->user_id);
    g_free(it->name);
    g_free(it->avatar);
    g_free(it->text);
    g_free(it->amount);
    for (int i = 0; i < it->n_emotes; i++) {
        g_free(it->emotes[i].id);
        g_free(it->emotes[i].url);
    }
    g_free(it->emotes);
    g_free(it);
}

/* ---- the index ------------------------------------------------------------ */

/* memmem is GNU's: first memchr on the needle's first byte. */
static const char *find(const char *hay, size_t len, const char *needle, size_t n)
{
    const char *end = hay + len;
    while (len >= n) {
        const char *p = memchr(hay, needle[0], len - n + 1);
        if (!p)
            return NULL;
        if (!memcmp(p, needle, n))
            return p;
        hay = p + 1;
        len = (size_t)(end - hay);
    }
    return NULL;
}

static const char *const message_keys[] = {"liveChatTextMessageRenderer",
                                           "liveChatPaidMessageRenderer",
                                           "liveChatPaidStickerRenderer",
                                           "liveChatMembershipItemRenderer", NULL};

static bool has_message(const char *line, size_t len)
{
    for (int i = 0; message_keys[i]; i++)
        if (find(line, len, message_keys[i], strlen(message_keys[i])))
            return true;
    return false;
}

/* "videoOffsetTimeMsec": "123456" (a string; negative before the start).
 * yt-dlp writes spaces after the colons; compact JSON has none. */
static bool line_time(const char *line, size_t len, int64_t *t)
{
    static const char key[] = "\"videoOffsetTimeMsec\"";
    const char *p = find(line, len, key, sizeof key - 1);
    if (!p)
        return false;
    const char *end = line + len;
    p += sizeof key - 1;
    while (p < end && (*p == ':' || *p == ' ' || *p == '"'))
        p++;
    char buf[24];
    size_t n = 0;
    while (p < end && n < sizeof buf - 1 && *p != '"' && *p != ',' && *p != '}')
        buf[n++] = *p++;
    buf[n] = '\0';
    char *stop;
    long long v = strtoll(buf, &stop, 10);
    if (stop == buf)
        return false;
    *t = v;
    return true;
}

static void push(kp_chatlog *log, int64_t t, uint64_t off, uint32_t len)
{
    if (log->n == log->cap) {
        log->cap = log->cap ? log->cap * 2 : 1024;
        log->index = g_renew(entry, log->index, log->cap);
    }
    log->index[log->n++] = (entry){t, off, len};
}

static int by_time(const void *a, const void *b)
{
    const entry *x = a, *y = b;
    if (x->t_ms != y->t_ms)
        return x->t_ms < y->t_ms ? -1 : 1;
    return x->off < y->off ? -1 : x->off > y->off;
}

static void build_index(kp_chatlog *log)
{
    const char *p = log->data, *end = log->data + log->size;
    int64_t last = 0;
    bool sorted = true;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (len > 0 && len < UINT32_MAX && has_message(p, len)) {
            int64_t t = last;
            line_time(p, len, &t); /* no time: the line before's */
            if (log->n && t < log->index[log->n - 1].t_ms)
                sorted = false;
            push(log, t, (uint64_t)(p - log->data), (uint32_t)len);
            last = t;
        }
        p += len + 1;
    }
    if (!sorted)
        qsort(log->index, log->n, sizeof(entry), by_time);
}

kp_chatlog *kp_chatlog_open(const char *path, char **err)
{
    GError *gerr = NULL;
    GMappedFile *map = g_mapped_file_new(path, FALSE, &gerr);
    if (!map) {
        if (err)
            *err = g_strdup(gerr->message);
        g_error_free(gerr);
        return NULL;
    }
    kp_chatlog *log = g_new0(kp_chatlog, 1);
    log->map = map;
    log->data = g_mapped_file_get_contents(map);
    log->size = g_mapped_file_get_length(map);
    build_index(log);
    return log;
}

void kp_chatlog_free(kp_chatlog *log)
{
    if (!log)
        return;
    g_free(log->index);
    if (log->map)
        g_mapped_file_unref(log->map);
    g_free(log);
}

typedef struct {
    kp_chatlog_done done;
    void *ud;
} open_job;

static void open_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src;
    (void)c;
    char *err = NULL;
    kp_chatlog *log = kp_chatlog_open(data, &err);
    if (log)
        g_task_return_pointer(task, log, (GDestroyNotify)kp_chatlog_free);
    else
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", err);
    g_free(err);
}

static void open_finished(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    open_job *job = data;
    GError *gerr = NULL;
    kp_chatlog *log = g_task_propagate_pointer(G_TASK(res), &gerr);
    job->done(log, gerr ? gerr->message : NULL, job->ud);
    g_clear_error(&gerr);
    g_free(job);
}

void kp_chatlog_open_async(const char *path, GCancellable *cancellable, kp_chatlog_done done,
                           void *ud)
{
    open_job *job = g_new0(open_job, 1);
    job->done = done;
    job->ud = ud;
    GTask *task = g_task_new(NULL, cancellable, open_finished, job);
    g_task_set_task_data(task, g_strdup(path), g_free);
    g_task_run_in_thread(task, open_thread);
    g_object_unref(task);
}

size_t kp_chatlog_count(const kp_chatlog *log)
{
    return log->n;
}

int64_t kp_chatlog_time(const kp_chatlog *log, size_t i)
{
    return log->index[i].t_ms;
}

size_t kp_chatlog_after(const kp_chatlog *log, int64_t t_ms)
{
    size_t lo = 0, hi = log->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (log->index[mid].t_ms <= t_ms)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* ---- decoding one line ---------------------------------------------------- */

typedef struct {
    char buf[MAX_TEXT];
    size_t len;
} text_buf;

/* Appends s without splitting a UTF-8 sequence. */
static void text_add(text_buf *t, const char *s)
{
    size_t n = strlen(s);
    if (t->len + n >= sizeof t->buf) {
        n = sizeof t->buf - 1 - t->len;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = '\0';
}

typedef struct {
    kp_chat_emote list[MAX_EMOTES];
    int n;
} emote_buf;

/* ".../abc=w48-h48-c-k-nd": the size sits after the last "=". */
static char *emote_url(const char *url, int px)
{
    const char *eq = strrchr(url, '=');
    int w, h, used = 0;
    if (eq && !strchr(eq, '/') && sscanf(eq, "=w%d-h%d%n", &w, &h, &used) == 2 && used > 0)
        return g_strdup_printf("%.*s=w%d-h%d%s", (int)(eq - url), url, px, px, eq + used);
    return g_strdup(url);
}

static void add_emote(emote_buf *eb, const cJSON *emoji, const text_buf *t, size_t start)
{
    const cJSON *last = NULL, *th;
    cJSON_ArrayForEach(th, kp_json_path(emoji, "image.thumbnails")) last = th;
    const char *url = last ? kp_json_str(last, "url") : NULL;
    const char *id = kp_json_str(emoji, "emojiId");
    if (!eb || eb->n == MAX_EMOTES || !url || !id)
        return;
    eb->list[eb->n++] = (kp_chat_emote){g_strdup(id), emote_url(url, EMOTE_PX), start, t->len - start};
}

/* Text of a {simpleText} or {runs:[{text}|{emoji}]} object. Standard emoji
 * become their Unicode character, channel emoji their :shortcut: (and go
 * to eb, when given, with their image). */
static void runs_text(const cJSON *obj, text_buf *t, emote_buf *eb)
{
    const char *simple = kp_json_str(obj, "simpleText");
    if (simple) {
        text_add(t, simple);
        return;
    }
    const cJSON *run;
    cJSON_ArrayForEach(run, cJSON_GetObjectItemCaseSensitive(obj, "runs"))
    {
        const char *s = kp_json_str(run, "text");
        const cJSON *emoji = NULL;
        bool custom = false;
        if (!s) {
            emoji = cJSON_GetObjectItemCaseSensitive(run, "emoji");
            custom = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(emoji, "isCustomEmoji"));
            s = custom ? kp_json_str(emoji, "shortcuts[0]") : NULL;
            if (!s)
                s = kp_json_str(emoji, "emojiId");
        }
        size_t start = t->len;
        if (s)
            text_add(t, s);
        if (custom && t->len > start)
            add_emote(eb, emoji, t, start);
    }
}

static unsigned parse_badges(const cJSON *item)
{
    unsigned badges = 0;
    const cJSON *b;
    cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(item, "authorBadges"))
    {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(b, "liveChatAuthorBadgeRenderer");
        const char *icon = kp_json_str(r, "icon.iconType");
        if (icon && !strcmp(icon, "OWNER"))
            badges |= KP_CHAT_OWNER;
        else if (icon && !strcmp(icon, "MODERATOR"))
            badges |= KP_CHAT_MOD;
        else if (icon && !strcmp(icon, "VERIFIED"))
            badges |= KP_CHAT_VERIFIED;
        else if (cJSON_GetObjectItemCaseSensitive(r, "customThumbnail"))
            badges |= KP_CHAT_MEMBERBADGE;
    }
    return badges;
}

/* The smallest picture, a few px is all the panel draws. */
static char *avatar_of(const cJSON *r)
{
    const cJSON *th = cJSON_GetArrayItem(kp_json_path(r, "authorPhoto.thumbnails"), 0);
    const char *url = th ? kp_json_str(th, "url") : NULL;
    return url ? g_strdup(url) : NULL;
}

/* One chat item (the value inside "item"); NULL if not a message. */
static kp_chat_item *make_item(const cJSON *item, int64_t t_ms)
{
    const cJSON *r;
    kp_chat_kind kind;
    text_buf text = {0}, amount = {0}, name = {0};
    emote_buf emotes = {.n = 0};

    if ((r = cJSON_GetObjectItemCaseSensitive(item, "liveChatTextMessageRenderer"))) {
        kind = KP_CHAT_TEXT;
        runs_text(cJSON_GetObjectItemCaseSensitive(r, "message"), &text, &emotes);
    } else if ((r = cJSON_GetObjectItemCaseSensitive(item, "liveChatPaidMessageRenderer")) ||
               (r = cJSON_GetObjectItemCaseSensitive(item, "liveChatPaidStickerRenderer"))) {
        kind = KP_CHAT_PAID;
        runs_text(cJSON_GetObjectItemCaseSensitive(r, "message"), &text, &emotes);
        runs_text(cJSON_GetObjectItemCaseSensitive(r, "purchaseAmountText"), &amount, NULL);
    } else if ((r = cJSON_GetObjectItemCaseSensitive(item, "liveChatMembershipItemRenderer"))) {
        kind = KP_CHAT_MEMBER;
        /* Milestones carry a message; new members only the header. */
        runs_text(cJSON_GetObjectItemCaseSensitive(r, "message"), &text, &emotes);
        if (text.len == 0)
            runs_text(cJSON_GetObjectItemCaseSensitive(r, "headerSubtext"), &text, NULL);
    } else {
        return NULL;
    }
    const char *user = kp_json_str(r, "authorExternalChannelId");
    runs_text(cJSON_GetObjectItemCaseSensitive(r, "authorName"), &name, NULL);
    if (!user || name.len == 0) {
        for (int i = 0; i < emotes.n; i++) {
            g_free(emotes.list[i].id);
            g_free(emotes.list[i].url);
        }
        return NULL;
    }
    kp_chat_item *it = g_new0(kp_chat_item, 1);
    it->t_ms = t_ms;
    it->kind = kind;
    it->badges = parse_badges(r);
    it->user_id = g_strdup(user);
    /* Handles come as "@name": the @ is noise. */
    it->name = g_strdup(name.buf[0] == '@' && name.buf[1] ? name.buf + 1 : name.buf);
    it->avatar = avatar_of(r);
    it->text = g_strdup(text.buf);
    it->amount = amount.len ? g_strdup(amount.buf) : NULL;
    it->n_emotes = emotes.n;
    it->emotes = emotes.n ? g_memdup2(emotes.list, sizeof(kp_chat_emote) * (gsize)emotes.n) : NULL;
    return it;
}

int kp_chatlog_decode(const kp_chatlog *log, size_t i, GPtrArray *out)
{
    const entry *e = &log->index[i];
    /* cJSON reads numbers with strtod: the C locale. */
    locale_t c = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    locale_t old = c ? uselocale(c) : (locale_t)0;
    cJSON *line = cJSON_ParseWithLength(log->data + e->off, e->len);
    if (c) {
        uselocale(old);
        freelocale(c);
    }
    int count = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, kp_json_path(line, "replayChatItemAction.actions"))
    {
        const cJSON *item = kp_json_path(a, "addChatItemAction.item");
        /* Held messages show as placeholders, later replaced. */
        if (!item)
            item = kp_json_path(a, "replaceChatItemAction.replacementItem");
        kp_chat_item *it = item ? make_item(item, e->t_ms) : NULL;
        if (it) {
            g_ptr_array_add(out, it);
            count++;
        }
    }
    cJSON_Delete(line);
    return count;
}

bool kp_chat_item_shown(const kp_chat_item *it, int filter)
{
    if (filter == 1)
        return it->kind != KP_CHAT_TEXT;
    if (filter == 2)
        return it->kind != KP_CHAT_TEXT ||
               (it->badges & (KP_CHAT_OWNER | KP_CHAT_MOD | KP_CHAT_MEMBERBADGE));
    return true;
}
