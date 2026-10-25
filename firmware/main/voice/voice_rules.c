#include "voice/voice_rules.h"
#include <math.h>
#include <string.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

size_t vr_wav_header(uint8_t out[VR_WAV_HDR], uint32_t data_bytes, uint32_t rate, uint16_t channels, uint16_t bits)
{
    if (rate == 0 || channels == 0 || bits != 16) return 0;
    uint16_t align = (uint16_t)(channels * bits / 8);
    memcpy(out, "RIFF", 4);
    put32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16);
    put16(out + 20, 1);               /* PCM */
    put16(out + 22, channels);
    put32(out + 24, rate);
    put32(out + 28, rate * align);
    put16(out + 32, align);
    put16(out + 34, bits);
    memcpy(out + 36, "data", 4);
    put32(out + 40, data_bytes);
    return VR_WAV_HDR;
}

size_t vr_downmix(const int16_t *in, size_t frames, int channels, int pick, int16_t *out)
{
    if (channels < 1) return 0;
    if (pick >= channels) pick = -1;
    for (size_t i = 0; i < frames; i++) {
        const int16_t *f = in + i * (size_t)channels;
        if (pick >= 0) { out[i] = f[pick]; continue; }
        int32_t sum = 0;
        for (int c = 0; c < channels; c++) sum += f[c];
        sum /= channels;
        out[i] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
    }
    return frames;
}

int vr_rms_dbfs(const int16_t *pcm, size_t n)
{
    if (!pcm || n == 0) return -96;
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)pcm[i] * pcm[i];
    if (acc <= 0) return -96;
    double db = 20.0 * log10(sqrt(acc / (double)n) / 32768.0);
    if (db < -96) return -96;
    return (int)lround(db);
}

int vr_meter(int dbfs)
{
    if (dbfs <= VR_METER_FLOOR_DBFS) return 0;
    if (dbfs >= 0) return 100;
    return (dbfs - VR_METER_FLOOR_DBFS) * 100 / -VR_METER_FLOOR_DBFS;
}

bool vr_heard(int loudest_block_dbfs) { return loudest_block_dbfs >= VR_SILENCE_DBFS; }

/* Ignore a key event that follows the previous one by less than the repeat gap (a held key repeats every 60 ms),
 * and any event within the guard window of the last toggle (the first repeat comes 500 ms after the press). */
vr_key_t vr_on_voice_key(vr_keys_t *k, bool recording, int64_t now_ms)
{
    bool repeat = k->seen && now_ms - k->last_ev_ms < VR_REPEAT_GAP_MS;
    bool guarded = k->seen && now_ms - k->last_toggle_ms < VR_TOGGLE_GUARD_MS;
    k->last_ev_ms = now_ms;
    if (repeat || guarded) return VR_KEY_IGNORE;
    k->seen = true;
    k->last_toggle_ms = now_ms;
    return recording ? VR_KEY_STOP : VR_KEY_START;
}

void vr_on_voice_ended(vr_keys_t *k, int64_t now_ms)
{
    k->seen = true;
    if (now_ms > k->last_toggle_ms) k->last_toggle_ms = now_ms;
}

/* The code point of the UTF-8 sequence at p (lead byte c >= 0xC0), or 0 for a malformed one. */
static uint32_t utf8_cp(const unsigned char *p)
{
    unsigned char c = p[0];
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    uint32_t cp = c & (0x3F >> n);
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | (p[i] & 0x3F);
    }
    return cp;
}

/* Spec 9.3 step 4 counts letters and digits only. Outside ASCII, the blocks whisper's non-speech output comes from
   are not letters: Latin-1 punctuation and signs (U+00A0-00BF, × ÷), General Punctuation (– — ‘ ’ “ ” …), arrows to
   dingbats (♪ ♫), CJK punctuation, variation selectors and emoji. Everything else (é, 日本, …) counts. */
static bool utf8_is_letter(uint32_t cp)
{
    if (cp == 0 || cp < 0xC0 || cp == 0xD7 || cp == 0xF7) return false;
    if (cp >= 0x2000 && cp <= 0x206F) return false;
    if (cp >= 0x2190 && cp <= 0x2BFF) return false;
    if (cp >= 0x3000 && cp <= 0x303F) return false;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return false;
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return false;
    return true;
}

size_t vr_clean_transcript(char *s)
{
    size_t w = 0;
    bool pending = false, alnum = false;
    for (const char *r = s; *r; ) {
        unsigned char c = (unsigned char)*r;
        char close = c == '[' ? ']' : c == '(' ? ')' : c == '*' ? '*' : 0;
        if (close) {
            const char *e = strchr(r + 1, close);
            if (e) { r = e + 1; continue; }     /* a tag: gone; unclosed falls through as text */
        }
        r++;
        if (c == '\n' || c == '\t' || c == ' ') { pending = true; continue; }
        if (c < 0x20 || c == 0x7f) continue;
        /* decoded before the write below: the sequence's own bytes are still ahead of w */
        if (c >= 0xC0 && utf8_is_letter(utf8_cp((const unsigned char *)r - 1))) alnum = true;
        if (pending && w > 0) s[w++] = ' ';
        pending = false;
        s[w++] = (char)c;
        if ((c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) alnum = true;
    }
    if (!alnum) w = 0;
    s[w] = 0;
    return w;
}

vr_merge_t vr_merge_draft(const char *draft, const char *text, char *out, size_t max_bytes)
{
    if (!draft) draft = "";
    if (!text) text = "";
    size_t dl = strlen(draft);
    bool cut = false;
    if (dl > max_bytes) {
        dl = max_bytes;
        while (dl > 0 && ((unsigned char)draft[dl] & 0xC0) == 0x80) dl--;
        cut = true;
    }
    memcpy(out, draft, dl);
    out[dl] = 0;
    size_t tl = strlen(text);
    if (tl == 0) return cut ? VR_MERGE_CUT : VR_MERGE_EMPTY;
    size_t sep = (dl > 0 && out[dl - 1] != ' ') ? 1 : 0;
    size_t avail = max_bytes > dl + sep ? max_bytes - dl - sep : 0;
    if (tl > avail) {
        cut = true;
        if (text[avail] != ' ') {
            size_t sp = avail;
            while (sp > 0 && text[sp - 1] != ' ') sp--;
            if (sp > 0) avail = sp - 1;          /* back to the last space that fits */
            else while (avail > 0 && ((unsigned char)text[avail] & 0xC0) == 0x80) avail--;
        }
        while (avail > 0 && text[avail - 1] == ' ') avail--;
        tl = avail;
    }
    if (tl == 0) return VR_MERGE_CUT;
    if (sep) out[dl++] = ' ';
    memcpy(out + dl, text, tl);
    out[dl + tl] = 0;
    return cut ? VR_MERGE_CUT : VR_MERGE_OK;
}

/* Exit codes: 120 and 121 are the pre-check codes of cc_cmd.h (CC_VOICE_RC_NO_WHISPER, CC_VOICE_RC_NO_MODEL);
 * not included here so this stays pure. */
vr_out_t vr_host_result(int exit_status)
{
    switch (exit_status) {
    case 0: return VR_OUT_TEXT;
    case 120: return VR_OUT_NO_WHISPER;
    case 121: return VR_OUT_NO_MODEL;
    case 3: return VR_OUT_BAD_MODEL;
    case 11: return VR_OUT_AUDIO_BAD;
    case 126: case 127: return VR_OUT_CANT_RUN;
    default: return VR_OUT_FAILED;
    }
}

str_id_t vr_out_text(vr_out_t o)
{
    switch (o) {
    case VR_OUT_BUSY: return STR_VOICE_BUSY;
    case VR_OUT_NO_FIELD: return STR_VOICE_NEEDS_FIELD;
    case VR_OUT_NO_LINK: return STR_SES_NOT_CONNECTED;
    case VR_OUT_NO_MEMORY: return STR_VOICE_NO_MEMORY;
    case VR_OUT_MIC_FAILED: return STR_VOICE_MIC_FAILED;
    case VR_OUT_TOO_SHORT: return STR_VOICE_TOO_SHORT;
    case VR_OUT_NO_SPEECH: return STR_VOICE_NO_SPEECH;
    case VR_OUT_NO_WHISPER: return STR_VOICE_NO_WHISPER;
    case VR_OUT_NO_MODEL: return STR_VOICE_NO_MODEL;
    case VR_OUT_BAD_MODEL: return STR_VOICE_BAD_MODEL;
    case VR_OUT_CANT_RUN: return STR_VOICE_CANT_RUN;
    case VR_OUT_AUDIO_BAD: return STR_VOICE_AUDIO_BAD;
    case VR_OUT_FAILED: return STR_VOICE_FAILED;
    case VR_OUT_TIMEOUT: return STR_VOICE_TIMEOUT;
    case VR_OUT_LINK_LOST: return STR_VOICE_LINK_LOST;
    default: return STR_COUNT;
    }
}
