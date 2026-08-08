// Headless GB/GBC audio renderer: plays a ROM through the vendored SameBoy
// core and writes a WAV, with the plugin's audio options as flags. Renders on
// the Mac in seconds so audio choices can be judged by ear without building or
// deploying anything to a device.
//
//   build: tools/audio_matrix.sh (compiles this against sameboy/Core)
//   usage: audio_matrix <rom> <out.wav> <seconds> <highpass 0|1|2> <dcblock 0|1> <fade_ms> [rate]
//
// highpass: 0 = OFF, 1 = ACCURATE (SameBoy's own default), 2 = REMOVE_DC_OFFSET
// dcblock:  the plugin's extra DC blocker (R = 0.9995) on top of the mode
// fade_ms:  the sink's fade-in from the first audible sample
#include <Core/gb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *out;
static long frames_written;
static long target_frames;
static uint32_t pixels[160 * 144];

static int opt_dcblock;
static double opt_dc_r = 0.9995;
static long opt_fade_samples;

static double dc_in_l, dc_in_r, dc_out_l, dc_out_r;
static long fade_done;
static int first_audible;

static void write_le16(FILE *f, int16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void write_le32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 0xff, f); }

static uint32_t g_rate = 48000;


static void wav_header(FILE *f, uint32_t data_bytes) {
    fwrite("RIFF", 1, 4, f); write_le32(f, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f); write_le32(f, 16);
    write_le16(f, 1); write_le16(f, 2);
    write_le32(f, g_rate); write_le32(f, g_rate * 4);
    write_le16(f, 4); write_le16(f, 16);
    fwrite("data", 1, 4, f); write_le32(f, data_bytes);
}

static void on_sample(GB_gameboy_t *gb, GB_sample_t *sample) {
    if (frames_written >= target_frames) return;

    double l = sample->left / 32768.0, r = sample->right / 32768.0;

    if (opt_dcblock) {
        const double R = opt_dc_r;
        dc_out_l = l - dc_in_l + R * dc_out_l;
        dc_out_r = r - dc_in_r + R * dc_out_r;
        dc_in_l = l; dc_in_r = r;
        l = dc_out_l; r = dc_out_r;
    }

    // The sink's fade-in: starts at the first audible sample, not at boot
    // silence, so a quiet intro can't consume the ramp.
    if (opt_fade_samples > 0 && fade_done < opt_fade_samples) {
        if (!first_audible && (l > 1e-5 || l < -1e-5)) first_audible = 1;
        if (first_audible) {
            double g = (double)fade_done / (double)opt_fade_samples;
            l *= g; r *= g;
            fade_done++;
        }
    }

    int16_t li = (int16_t)(l * 32767.0), ri = (int16_t)(r * 32767.0);
    write_le16(out, li); write_le16(out, ri);
    frames_written++;
}

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s <rom> <out.wav> <seconds> <highpass 0|1|2> <dcblock 0|1> <fade_ms>\n", argv[0]);
        return 1;
    }
    const char *rom = argv[1], *wav = argv[2];
    double seconds = atof(argv[3]);
    int highpass = atoi(argv[4]);
    opt_dcblock = atoi(argv[5]);
    if (argc > 7) g_rate = (uint32_t)atoi(argv[7]);
    if (argc > 8) opt_dc_r = atof(argv[8]);
    opt_fade_samples = (long)(atof(argv[6]) / 1000.0 * g_rate);
    target_frames = (long)(seconds * g_rate);

    // The boot ROM ships with the plugin; a GBC ROM needs the CGB one.
    const char *boot = strstr(rom, ".gbc") ? "native/firmware/sameboy_cgb_boot.bin"
                                           : "native/firmware/sameboy_dmg_boot.bin";
    FILE *bf = fopen(boot, "rb");
    if (!bf) { fprintf(stderr, "boot rom missing: %s (run from the repo root)\n", boot); return 1; }
    static uint8_t boot_buf[4096];
    size_t boot_len = fread(boot_buf, 1, sizeof boot_buf, bf);
    fclose(bf);

    out = fopen(wav, "wb");
    if (!out) { perror("out"); return 1; }
    wav_header(out, 0);

    GB_gameboy_t *gb = GB_init(GB_alloc(), strstr(rom, ".gbc") ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);
    GB_load_boot_rom_from_buffer(gb, boot_buf, boot_len);
    GB_set_rgb_encode_callback(gb, rgb_encode);
    GB_set_pixels_output(gb, pixels);
    GB_set_sample_rate(gb, g_rate);
    GB_set_highpass_filter_mode(gb, highpass == 0 ? GB_HIGHPASS_OFF
                                  : highpass == 1 ? GB_HIGHPASS_ACCURATE
                                                  : GB_HIGHPASS_REMOVE_DC_OFFSET);
    GB_apu_set_sample_callback(gb, on_sample);
    if (GB_load_rom(gb, rom) != 0) { fprintf(stderr, "rom load failed: %s\n", rom); return 1; }

    // Press Start twice on the way through so title screens advance into music.
    long frame = 0;
    while (frames_written < target_frames) {
        GB_run_frame(gb);
        frame++;
        if (frame == 400 || frame == 700) GB_set_key_state(gb, GB_KEY_START, true);
        if (frame == 415 || frame == 715) GB_set_key_state(gb, GB_KEY_START, false);
        if (frame > 60 * 60 * 5) break;   // safety: 5 emulated minutes
    }

    uint32_t data_bytes = (uint32_t)(frames_written * 4);
    fseek(out, 0, SEEK_SET);
    wav_header(out, data_bytes);
    fclose(out);
    fprintf(stderr, "%s: %.1fs @ %u Hz (highpass=%d dcblock=%d fade=%sms)\n",
            wav, (double)frames_written / g_rate, g_rate, highpass, opt_dcblock, argv[6]);
    return 0;
}
