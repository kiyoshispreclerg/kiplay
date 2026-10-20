/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "midi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TSF_IMPLEMENTATION
#include "tsf.h"
#define TML_IMPLEMENTATION
#include "tml.h"

/* What rings after the last message (reverb tails, slow releases). */
#define TAIL_MS 1500
/* Headroom for chords: the sum of voices clips otherwise. */
#define GAIN_DB (-4.0f)
/* The General MIDI drum kit is channel 10. */
#define DRUM_CHANNEL 9

/* Tried in this order when none is set: the smallest first, as the
 * SoundFont costs twice its size in memory. */
static const char *const usual[] = {
    "/usr/share/sounds/sf2/TimGM6mb.sf2",
    "/usr/share/sounds/sf2/default-GM.sf2",
    "/usr/share/sounds/sf2/FluidR3_GM.sf2",
    "/usr/share/soundfonts/default.sf2",
    "/usr/share/soundfonts/FluidR3_GM.sf2",
    "/usr/share/sounds/sf2/FluidR3_GS.sf2",
    NULL,
};

static char *want; /* set by ka_midi_set_soundfont */
static tsf *font;  /* the SoundFont, loaded; files hold copies that share its samples */
static char *font_path;
static int holders;

struct ka_midi {
    tml_message *first, *cur;
    tsf *synth;
    int rate;
    int length_ms; /* the last message's time */
    int64_t pos;   /* frames rendered */
};

void ka_midi_set_soundfont(const char *path)
{
    free(want);
    want = path && *path ? strdup(path) : NULL;
}

bool ka_midi_is(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.');
    if (!dot)
        return false;
    return !strcasecmp(dot, ".mid") || !strcasecmp(dot, ".midi") || !strcasecmp(dot, ".kar");
}

static bool readable(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* The SoundFont file to load, or NULL. */
static const char *find_font(void)
{
    if (want)
        return readable(want) ? want : NULL;
    for (int i = 0; usual[i]; i++)
        if (readable(usual[i]))
            return usual[i];
    return NULL;
}

static bool load_font(char *err, int errsize)
{
    const char *path = find_font();
    if (!path) {
        snprintf(err, (size_t)errsize, want ? "cannot read the SoundFont %s" : "no SoundFont found",
                 want ? want : "");
        return false;
    }
    if (font && font_path && !strcmp(font_path, path))
        return true;
    if (font) {
        /* Another one was asked for: the open files keep theirs. */
        tsf_close(font);
        font = NULL;
    }
    font = tsf_load_filename(path);
    if (!font) {
        snprintf(err, (size_t)errsize, "cannot load the SoundFont %s", path);
        return false;
    }
    free(font_path);
    font_path = strdup(path);
    return true;
}

void ka_midi_trim(void)
{
    if (holders > 0 || !font)
        return;
    tsf_close(font);
    font = NULL;
    free(font_path);
    font_path = NULL;
}

ka_midi *ka_midi_open(const char *path, int rate, char *err, int errsize)
{
    ka_midi *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(err, (size_t)errsize, "out of memory");
        return NULL;
    }
    m->rate = rate;
    m->first = m->cur = tml_load_filename(path);
    if (!m->first) {
        snprintf(err, (size_t)errsize, "not a valid MIDI file");
        free(m);
        return NULL;
    }
    unsigned length = 0;
    tml_get_info(m->first, NULL, NULL, NULL, NULL, &length);
    m->length_ms = (int)length;
    if (rate <= 0)
        return m;
    if (!load_font(err, errsize)) {
        ka_midi_close(m);
        return NULL;
    }
    m->synth = tsf_copy(font);
    if (!m->synth) {
        snprintf(err, (size_t)errsize, "out of memory");
        ka_midi_close(m);
        return NULL;
    }
    holders++;
    tsf_set_output(m->synth, TSF_STEREO_INTERLEAVED, rate, GAIN_DB);
    /* Drums without a program change, as General MIDI has it. */
    tsf_channel_set_presetnumber(m->synth, DRUM_CHANNEL, 0, 1);
    return m;
}

void ka_midi_close(ka_midi *m)
{
    if (!m)
        return;
    if (m->synth) {
        tsf_close(m->synth);
        holders--;
    }
    tml_free(m->first);
    free(m);
}

/* One message into the synth; notes only when `notes`. */
static void apply(ka_midi *m, const tml_message *msg, bool notes)
{
    switch (msg->type) {
    case TML_PROGRAM_CHANGE:
        tsf_channel_set_presetnumber(m->synth, msg->channel, msg->program, msg->channel == DRUM_CHANNEL);
        break;
    case TML_CONTROL_CHANGE:
        tsf_channel_midi_control(m->synth, msg->channel, msg->control, msg->control_value);
        break;
    case TML_PITCH_BEND:
        tsf_channel_set_pitchwheel(m->synth, msg->channel, msg->pitch_bend);
        break;
    case TML_NOTE_ON:
        if (notes)
            tsf_channel_note_on(m->synth, msg->channel, msg->key, msg->velocity / 127.0f);
        break;
    case TML_NOTE_OFF:
        if (notes)
            tsf_channel_note_off(m->synth, msg->channel, msg->key);
        break;
    default:
        break;
    }
}

long ka_midi_read(ka_midi *m, int16_t *out, long max)
{
    int64_t total = (int64_t)(m->length_ms + TAIL_MS) * m->rate / 1000;
    if (m->pos >= total)
        return 0;
    if (max > total - m->pos)
        max = (long)(total - m->pos);
    long done = 0;
    while (done < max) {
        long block = max - done < TSF_RENDER_EFFECTSAMPLEBLOCK ? max - done : TSF_RENDER_EFFECTSAMPLEBLOCK;
        /* The messages that fall in this block start with it. */
        while (m->cur && (int64_t)m->cur->time * m->rate < (m->pos + block) * 1000) {
            apply(m, m->cur, true);
            m->cur = m->cur->next;
        }
        tsf_render_short(m->synth, out + done * 2, (int)block, 0);
        m->pos += block;
        done += block;
    }
    return done;
}

bool ka_midi_seek(ka_midi *m, double seconds)
{
    if (seconds < 0)
        seconds = 0;
    tsf_reset(m->synth);
    tsf_channel_set_presetnumber(m->synth, DRUM_CHANNEL, 0, 1);
    m->pos = (int64_t)(seconds * m->rate);
    /* What the channels were told before this point, without the notes. */
    m->cur = m->first;
    while (m->cur && (int64_t)m->cur->time * m->rate < m->pos * 1000) {
        apply(m, m->cur, false);
        m->cur = m->cur->next;
    }
    return true;
}

double ka_midi_duration(const ka_midi *m)
{
    return (m->length_ms + TAIL_MS) / 1000.0;
}
