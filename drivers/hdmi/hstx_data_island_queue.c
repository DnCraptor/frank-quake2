#include "hstx_data_island_queue.h"

#include <string.h>

#include "pico.h"

/* frank-castle: audio goes out over I2S and nothing is ever queued here,
 * only the silent packet is sent, so the ring is kept minimal (was 256) */
#define DI_RING_BUFFER_SIZE 2 /* frank-quake2: unused too, audio packets are made on the fly */
static hstx_data_island_t di_ring_buffer[DI_RING_BUFFER_SIZE];
static volatile uint32_t di_ring_head = 0;
static volatile uint32_t di_ring_tail = 0;

// Single pre-encoded silent audio packet (fixed B-frame flags).
static hstx_data_island_t silence_packet;

// Audio timing state (default 48kHz, 525 lines for 480p)
static uint32_t audio_sample_accum = 0; // Fixed-point accumulator
static uint32_t cached_v_total_lines = 525;
#define DEFAULT_SAMPLES_PER_FRAME (48000 / 60)
static uint32_t samples_per_line_fp = (DEFAULT_SAMPLES_PER_FRAME << 16) / 525;

// Limit accumulator to avoid overflow if we run dry.
// Clamping to 1 packet (plus a tiny margin is implicit) ensures we don't burst.
#define MAX_AUDIO_ACCUM (4 << 16)

void hstx_di_queue_init(void)
{
    di_ring_head = 0;
    di_ring_tail = 0;
    audio_sample_accum = 0;
    // Build a single silent audio packet.
    hstx_packet_t packet;
    audio_sample_t samples[4] = {0};
    (void)hstx_packet_set_audio_samples(&packet, samples, 4, 0);
    hstx_encode_data_island(&silence_packet, &packet, false, true);
}

void hstx_di_queue_set_sample_rate(uint32_t sample_rate)
{
    uint32_t samples_per_frame = sample_rate / 60;
    samples_per_line_fp = (samples_per_frame << 16) / cached_v_total_lines;
}

void hstx_di_queue_set_v_total(uint32_t v_total)
{
    cached_v_total_lines = v_total;
}

void hstx_di_queue_set_samples_per_line_fp(uint32_t value)
{
    samples_per_line_fp = value;
}

bool hstx_di_queue_push(const hstx_data_island_t *island)
{
    uint32_t next_head = (di_ring_head + 1) % DI_RING_BUFFER_SIZE;
    if (next_head == di_ring_tail)
        return false;

    di_ring_buffer[di_ring_head] = *island;
    di_ring_head = next_head;
    return true;
}

uint32_t hstx_di_queue_get_level(void)
{
    uint32_t head = di_ring_head;
    uint32_t tail = di_ring_tail;
    if (head >= tail)
        return head - tail;
    return DI_RING_BUFFER_SIZE + head - tail;
}

/* ------------------------------------------------------------------ */
/* Audio packets made on the fly (frank-quake2)                        */
/* ------------------------------------------------------------------ */

extern uint16_t hstx_ter_c4[16];   /* hstx_packet.c, in SRAM */
extern uint8_t hstx_bch_table[256];

static hstx_audio_source_t audio_source;
/* the next packet, made at the end of the IRQ that handed out the last one
 * (hstx_di_queue_post): after the DMA is set up again, the tight part */
static hstx_data_island_t audio_island;
static volatile bool audio_island_used;
/* guard bands and the header in lane 0, the same for every packet but for
 * the B flag (a new 192-sample block: every 48 packets): [1] has it */
static uint32_t audio_tmpl[2][HSTX_DATA_ISLAND_WORDS];
static uint32_t audio_packets; /* mod 48 */
static uint32_t audio_prev;    /* the source frame before */

void hstx_di_queue_set_audio_source(hstx_audio_source_t source)
{
    audio_source = NULL;
    for (int b = 0; b < 2; b++) {
        audio_sample_t zero[4] = {0};
        hstx_packet_t packet;
        hstx_data_island_t island;
        hstx_packet_set_audio_samples(&packet, zero, 4, b ? 0 : 1);
        hstx_encode_data_island(&island, &packet, false, true);
        for (int i = 0; i < HSTX_DATA_ISLAND_WORDS; i++)
            audio_tmpl[b][i] = (i >= 2 && i < 34) ? island.words[i] & 0x3ff : island.words[i];
    }
    audio_packets = 0;
    audio_prev = 0;
    audio_island = silence_packet;
    audio_island_used = false;
    __asm volatile("dmb" ::: "memory");
    audio_source = source;
}

static inline uint32_t parity8(uint32_t v)
{
    v ^= v >> 4;
    return (0x6996u >> (v & 0xf)) & 1;
}

static void __no_inline_not_in_flash_func(make_audio_packet)(void)
{
    uint32_t cur = audio_source();
    int pl = (int16_t)audio_prev, pr = (int16_t)(audio_prev >> 16);
    int dl = (int16_t)cur - pl, dr = (int16_t)(cur >> 16) - pr;
    audio_prev = cur;

    uint8_t sp[4][8];
    for (int k = 0; k < 4; k++) {
        uint8_t *d = sp[k];
        int l = pl + dl * (k + 1) / 4, r = pr + dr * (k + 1) / 4;
        d[0] = 0;
        d[1] = (uint8_t)l;
        d[2] = (uint8_t)(l >> 8);
        d[3] = 0;
        d[4] = (uint8_t)r;
        d[5] = (uint8_t)(r >> 8);
        d[6] = (uint8_t)(parity8(d[1] ^ d[2]) << 3 | parity8(d[4] ^ d[5]) << 7);
        uint8_t v = hstx_bch_table[0];
        for (int i = 1; i < 7; i++) v = hstx_bch_table[d[i] ^ v];
        d[7] = v;
    }

    const uint32_t *t = audio_tmpl[audio_packets == 0];
    if (++audio_packets == 48) audio_packets = 0;
    uint32_t *w = audio_island.words;
    w[0] = t[0];
    w[1] = t[1];
    w[34] = t[34];
    w[35] = t[35];
    for (int i = 0; i < 8; i++) {
        uint32_t v = sp[0][i] | sp[1][i] << 8 | sp[2][i] << 16 | (uint32_t)sp[3][i] << 24;
        uint32_t x = (v ^ (v >> 7)) & 0x00aa00aa;
        v = v ^ x ^ (x << 7);
        x = (v ^ (v >> 14)) & 0x0000cccc;
        v = v ^ x ^ (x << 14);
        const uint32_t *ti = t + 2 + i * 4;
        uint32_t *wi = w + 2 + i * 4;
        wi[0] = ti[0] | (uint32_t)hstx_ter_c4[v & 0xf] << 10 | (uint32_t)hstx_ter_c4[(v >> 8) & 0xf] << 20;
        wi[1] = ti[1] | (uint32_t)hstx_ter_c4[(v >> 16) & 0xf] << 10 | (uint32_t)hstx_ter_c4[(v >> 24) & 0xf] << 20;
        wi[2] = ti[2] | (uint32_t)hstx_ter_c4[(v >> 4) & 0xf] << 10 | (uint32_t)hstx_ter_c4[(v >> 12) & 0xf] << 20;
        wi[3] = ti[3] | (uint32_t)hstx_ter_c4[(v >> 20) & 0xf] << 10 | (uint32_t)hstx_ter_c4[(v >> 28) & 0xf] << 20;
    }
}

/* at the end of the HSTX IRQ: the next packet, if this one went out */
void __not_in_flash_func(hstx_di_queue_post)(void)
{
    if (audio_island_used && audio_source) {
        audio_island_used = false;
        make_audio_packet();
    }
}

void __scratch_x("") hstx_di_queue_tick(void)
{
    audio_sample_accum += samples_per_line_fp;
}

const uint32_t *__scratch_x("") hstx_di_queue_get_audio_packet(void)
{
    // Check if it's time to send a 4-sample audio packet (every ~2.6 lines)
    if (audio_sample_accum >= (4 << 16)) {
        audio_sample_accum -= (4 << 16);
        if (audio_source) {
            audio_island_used = true; /* copied into the line at once */
            return audio_island.words;
        }
        if (di_ring_tail != di_ring_head) {
            const uint32_t *words = di_ring_buffer[di_ring_tail].words;
            di_ring_tail = (di_ring_tail + 1) % DI_RING_BUFFER_SIZE;
            return words;
        }
        // Queue is empty: return a pre-encoded silent packet to keep HDMI audio active.
        return silence_packet.words;
    }
    return NULL;
}
