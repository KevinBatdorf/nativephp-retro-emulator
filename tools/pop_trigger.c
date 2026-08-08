// Which APU register write causes the pop?
//
//   build: see tools/pop_trigger.sh
//   usage: pop_trigger <rom> <out.wav> <writes.csv> <seconds> <highpass 0|1|2>
//
// Renders through the vendored SameBoy core exactly as audio_matrix does, and
// additionally logs every write to the sound registers (0xFF10-0xFF3F) against
// the output sample it landed on. Correlating those writes with the level steps
// says which register moves the level, instead of guessing from the waveform.
#include <Core/gb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *out;
static FILE *csv;
static long frames_written;
static long target_frames;
static uint32_t pixels[160 * 144];
static uint32_t g_rate = 48000;

static void write_le16(FILE *f, int16_t v) { fputc(v & 0xff, f); fputc((v >> 8) & 0xff, f); }
static void write_le32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 0xff, f); }

static void wav_header(FILE *f, uint32_t data_bytes) {
    fwrite("RIFF", 1, 4, f); write_le32(f, 36 + data_bytes);
    fwrite("WAVEfmt ", 1, 8, f); write_le32(f, 16);
    write_le16(f, 1); write_le16(f, 2);
    write_le32(f, g_rate); write_le32(f, g_rate * 4);
    write_le16(f, 4); write_le16(f, 16);
    fwrite("data", 1, 4, f); write_le32(f, data_bytes);
}

// Names so the output reads as registers rather than addresses.
static const char *reg_name(uint16_t a) {
    switch (a) {
        case 0xFF10: return "NR10_sweep";
        case 0xFF11: return "NR11_duty_len";
        case 0xFF12: return "NR12_envelope";
        case 0xFF13: return "NR13_freq_lo";
        case 0xFF14: return "NR14_freq_hi_trigger";
        case 0xFF16: return "NR21_duty_len";
        case 0xFF17: return "NR22_envelope";
        case 0xFF18: return "NR23_freq_lo";
        case 0xFF19: return "NR24_freq_hi_trigger";
        case 0xFF1A: return "NR30_wave_dac";
        case 0xFF1B: return "NR31_length";
        case 0xFF1C: return "NR32_wave_level";
        case 0xFF1D: return "NR33_freq_lo";
        case 0xFF1E: return "NR34_freq_hi_trigger";
        case 0xFF20: return "NR41_length";
        case 0xFF21: return "NR42_envelope";
        case 0xFF22: return "NR43_poly";
        case 0xFF23: return "NR44_trigger";
        case 0xFF24: return "NR50_master_volume";
        case 0xFF25: return "NR51_panning";
        case 0xFF26: return "NR52_power";
        default: return (a >= 0xFF30 && a <= 0xFF3F) ? "WAVE_RAM" : "other";
    }
}

static bool on_write(GB_gameboy_t *gb, uint16_t addr, uint8_t data) {
    if (addr >= 0xFF10 && addr <= 0xFF3F) {
        fprintf(csv, "%ld,%04X,%s,%02X\n", frames_written, addr, reg_name(addr), data);
    }
    return true;
}

static void on_sample(GB_gameboy_t *gb, GB_sample_t *sample) {
    if (frames_written >= target_frames) return;
    write_le16(out, sample->left);
    write_le16(out, sample->right);
    frames_written++;
}

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: %s <rom> <out.wav> <writes.csv> <seconds> <highpass 0|1|2>\n", argv[0]);
        return 1;
    }
    const char *rom = argv[1];
    double seconds = atof(argv[4]);
    int highpass = atoi(argv[5]);
    target_frames = (long)(seconds * g_rate);

    const char *boot = strstr(rom, ".gbc") ? "native/firmware/sameboy_cgb_boot.bin"
                                           : "native/firmware/sameboy_dmg_boot.bin";
    FILE *bf = fopen(boot, "rb");
    if (!bf) { fprintf(stderr, "boot rom missing: %s (run from the repo root)\n", boot); return 1; }
    static uint8_t boot_buf[4096];
    size_t boot_len = fread(boot_buf, 1, sizeof boot_buf, bf);
    fclose(bf);

    out = fopen(argv[2], "wb");
    csv = fopen(argv[3], "w");
    if (!out || !csv) { perror("open"); return 1; }
    wav_header(out, 0);
    fprintf(csv, "sample,addr,name,value\n");

    GB_gameboy_t *gb = GB_init(GB_alloc(), strstr(rom, ".gbc") ? GB_MODEL_CGB_E : GB_MODEL_DMG_B);
    GB_load_boot_rom_from_buffer(gb, boot_buf, boot_len);
    GB_set_rgb_encode_callback(gb, rgb_encode);
    GB_set_pixels_output(gb, pixels);
    GB_set_sample_rate(gb, g_rate);
    GB_set_highpass_filter_mode(gb, highpass == 0 ? GB_HIGHPASS_OFF
                                  : highpass == 1 ? GB_HIGHPASS_ACCURATE
                                                  : GB_HIGHPASS_REMOVE_DC_OFFSET);
    GB_apu_set_sample_callback(gb, on_sample);
    GB_set_write_memory_callback(gb, on_write);
    if (GB_load_rom(gb, rom) != 0) { fprintf(stderr, "rom load failed: %s\n", rom); return 1; }

    long frame = 0;
    while (frames_written < target_frames) {
        GB_run_frame(gb);
        frame++;
        if (frame == 400 || frame == 700) GB_set_key_state(gb, GB_KEY_START, true);
        if (frame == 415 || frame == 715) GB_set_key_state(gb, GB_KEY_START, false);
        if (frame > 60 * 60 * 5) break;
    }

    uint32_t data_bytes = (uint32_t)(frames_written * 4);
    fseek(out, 0, SEEK_SET);
    wav_header(out, data_bytes);
    fclose(out);
    fclose(csv);
    fprintf(stderr, "%s: %.1fs @ %u Hz (highpass=%d)\n",
            argv[2], (double)frames_written / g_rate, g_rate, highpass);
    return 0;
}
