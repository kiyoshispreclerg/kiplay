/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "../harness.h"
#include "chatlog.h"

/* Synthetic lines in the shape yt-dlp saves (one JSON object per line). The
 * second is out of order, the fourth a placeholder, the fifth a ticker. */
static const char sample[] =
    "{\"replayChatItemAction\":{\"actions\":[{\"addChatItemAction\":{\"item\":{"
    "\"liveChatTextMessageRenderer\":{\"message\":{\"runs\":[{\"text\":\"hello \"},"
    "{\"emoji\":{\"emojiId\":\"UC1/abc\",\"shortcuts\":[\":_wave:\"],\"isCustomEmoji\":true,"
    "\"image\":{\"thumbnails\":[{\"url\":\"https://x/y=w24-h24-c-k-nd\"},"
    "{\"url\":\"https://x/y=w48-h48-c-k-nd\"}]}}},{\"emoji\":{\"emojiId\":\"🍎\"}}]},"
    "\"authorName\":{\"simpleText\":\"@alice\"},\"authorExternalChannelId\":\"UCa\","
    "\"authorPhoto\":{\"thumbnails\":[{\"url\":\"https://p/a=s32\"}]},"
    "\"authorBadges\":[{\"liveChatAuthorBadgeRenderer\":{\"icon\":{\"iconType\":\"MODERATOR\"}}}]}}}}],"
    "\"videoOffsetTimeMsec\":\"5000\"}}\n"
    "{\"replayChatItemAction\":{\"actions\":[{\"addChatItemAction\":{\"item\":{"
    "\"liveChatPaidMessageRenderer\":{\"message\":{\"runs\":[{\"text\":\"thanks\"}]},"
    "\"purchaseAmountText\":{\"simpleText\":\"¥500\"},"
    "\"authorName\":{\"simpleText\":\"bob\"},\"authorExternalChannelId\":\"UCb\"}}}}],"
    "\"videoOffsetTimeMsec\":\"2000\"}}\n"
    "{\"replayChatItemAction\":{\"actions\":[{\"addChatItemAction\":{\"item\":{"
    "\"liveChatTextMessageRenderer\":{\"message\":{\"simpleText\":\"early\"},"
    "\"authorName\":{\"simpleText\":\"c\"},\"authorExternalChannelId\":\"UCc\"}}}}],"
    "\"videoOffsetTimeMsec\":\"-3000\"}}\n"
    "{\"replayChatItemAction\":{\"actions\":[{\"addChatItemAction\":{\"item\":{"
    "\"liveChatPlaceholderItemRenderer\":{\"id\":\"x\"}}}}],\"videoOffsetTimeMsec\":\"6000\"}}\n"
    "{\"replayChatItemAction\":{\"actions\":[{\"addLiveChatTickerItemAction\":{}}],"
    "\"videoOffsetTimeMsec\":\"7000\"}}\n"
    "{\"replayChatItemAction\":{\"actions\":[{\"addChatItemAction\":{\"item\":{"
    "\"liveChatTextMessageRenderer\":{\"message\":{\"simpleText\":\"two in a line\"},"
    "\"authorName\":{\"simpleText\":\"d\"},\"authorExternalChannelId\":\"UCd\"}}}},"
    "{\"addChatItemAction\":{\"item\":{\"liveChatTextMessageRenderer\":{"
    "\"message\":{\"simpleText\":\"second\"},\"authorName\":{\"simpleText\":\"e\"},"
    "\"authorExternalChannelId\":\"UCe\"}}}}],\"videoOffsetTimeMsec\": \"9000\"}}\n";

static kp_chatlog *open_sample(char **path)
{
    GError *e = NULL;
    int fd = g_file_open_tmp("kp-chat-XXXXXX.json", path, &e);
    g_assert(fd >= 0);
    close(fd);
    g_assert(g_file_set_contents(*path, sample, -1, NULL));
    char *err = NULL;
    kp_chatlog *log = kp_chatlog_open(*path, &err);
    g_assert(log != NULL);
    return log;
}

TEST(indexes_only_messages_in_time_order)
{
    char *path;
    kp_chatlog *log = open_sample(&path);
    CHECK_INT_EQ(kp_chatlog_count(log), 4); /* placeholder and ticker left out */
    CHECK_INT_EQ(kp_chatlog_time(log, 0), -3000);
    CHECK_INT_EQ(kp_chatlog_time(log, 1), 2000);
    CHECK_INT_EQ(kp_chatlog_time(log, 3), 9000);
    CHECK_INT_EQ(kp_chatlog_after(log, -4000), 0);
    CHECK_INT_EQ(kp_chatlog_after(log, 2000), 2); /* after, not at */
    CHECK_INT_EQ(kp_chatlog_after(log, 100000), 4);
    kp_chatlog_free(log);
    g_unlink(path);
    g_free(path);
}

TEST(decodes_text_emotes_badges_and_paid)
{
    char *path;
    kp_chatlog *log = open_sample(&path);
    GPtrArray *out = g_ptr_array_new();

    CHECK_INT_EQ(kp_chatlog_decode(log, 2, out), 1); /* alice, 5000 */
    kp_chat_item *a = g_ptr_array_index(out, 0);
    CHECK_STR_EQ(a->name, "alice");
    CHECK_STR_EQ(a->user_id, "UCa");
    CHECK_INT_EQ(a->t_ms, 5000);
    CHECK(a->kind == KP_CHAT_TEXT);
    CHECK(a->badges & KP_CHAT_MOD);
    CHECK_STR_EQ(a->avatar, "https://p/a=s32");
    CHECK_STR_EQ(a->text, "hello :_wave:🍎");
    CHECK_INT_EQ(a->n_emotes, 1);
    CHECK_STR_EQ(a->emotes[0].id, "UC1/abc");
    CHECK_STR_EQ(a->emotes[0].url, "https://x/y=w48-h48-c-k-nd");
    CHECK_INT_EQ(a->emotes[0].start, 6);
    CHECK_INT_EQ(a->emotes[0].len, 7);

    CHECK_INT_EQ(kp_chatlog_decode(log, 1, out), 1); /* bob, 2000 */
    kp_chat_item *b = g_ptr_array_index(out, 1);
    CHECK(b->kind == KP_CHAT_PAID);
    CHECK_STR_EQ(b->amount, "¥500");
    CHECK_STR_EQ(b->text, "thanks");

    CHECK_INT_EQ(kp_chatlog_decode(log, 3, out), 2); /* two actions in a line */
    CHECK_STR_EQ(((kp_chat_item *)g_ptr_array_index(out, 3))->text, "second");

    /* The filters. */
    CHECK(kp_chat_item_shown(a, 0));
    CHECK(!kp_chat_item_shown(a, 1));
    CHECK(kp_chat_item_shown(b, 1));

    g_ptr_array_set_free_func(out, (GDestroyNotify)kp_chat_item_free);
    g_ptr_array_free(out, TRUE);
    kp_chatlog_free(log);
    g_unlink(path);
    g_free(path);
}

TEST(open_reports_a_missing_file)
{
    char *err = NULL;
    CHECK(kp_chatlog_open("/nonexistent/kiplay-chat.json", &err) == NULL);
    CHECK(err != NULL);
    g_free(err);
}

int main(void)
{
    RUN(indexes_only_messages_in_time_order);
    RUN(decodes_text_emotes_badges_and_paid);
    RUN(open_reports_a_missing_file);
    return harness_report();
}
