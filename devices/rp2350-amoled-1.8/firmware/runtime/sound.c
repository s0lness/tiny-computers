/*
 * sound: implementation. See sound.h for the API and the ownership decision
 * (everything i2c1 happens once, from sound_init(), before core1 exists).
 *
 * PINS. AGENTS.md's pin table lists DOUT/DIN/MCLK/BCLK (20/21/22/24) but no
 * LRCK (word select), which I2S cannot function without. Traced on the
 * published schematic (files.waveshare.com/wiki/RP2350-Touch-AMOLED-1.8/
 * RP2350-Touch-AMOLED-1.8.pdf, "Codec" block) by matching each net label's
 * y-coordinate against the ES8311's own pin-side label in the same row:
 *
 *   y=518.7  GPIO22 <-> I2S_MCLK
 *   y=521.5  GPIO24 <-> I2S_SCLK   (BCLK)
 *   y=524.4  GPIO21 <-> I2S_ASDOUT (ADC serial data OUT of the codec = this
 *                                   board's DIN; unused here, no mic capture)
 *   y=527.3  GPIO23 <-> I2S_LRCK   (the missing pin - AGENTS.md's table
 *                                   should be corrected to include it)
 *   y=530.2  GPIO20 <-> I2S_DSDIN  (DAC serial data IN to the codec = DOUT)
 *
 * So DOUT/DIN/MCLK/LRCK/BCLK are 20/21/22/23/24, contiguous. AGENTS.md's
 * table is missing GPIO23 = LRCK; fix that alongside this file.
 *
 * The codec's CE pin (address select) is pulled to AGND through a 10k
 * resistor (R17, same schematic block) - CE low selects I2C address 0x18 per
 * the ES8311 datasheet, confirming the address the task brief already stated
 * and matching Waveshare's own ES8311 example for the sibling
 * RP2350-Touch-AMOLED-1.75 board (github.com/waveshareteam/
 * RP2350-Touch-AMOLED-1.75, examples/C/02_ES8311/lib/es8311/es8311.h,
 * ES8311_I2C_ADDR 0x18).
 *
 * NO EXTERNAL MCLK. The schematic wires GPIO22 to the codec's MCLK pin, but
 * this file does not drive it: the ES8311 can derive its internal master
 * clock from the bit clock instead (register 0x01 bit 7, "select BCLK/SCK
 * pin" - see es8311_bring_up() below), which needs only the two pins this
 * design already drives as PIO side-set outputs (LRCK, BCLK) plus DOUT, one
 * fewer PIO output and one fewer clock-divider computation than generating a
 * clean MCLK square wave would need. Confirmed against a proven, shipped
 * driver (espressif/esp-bsp, components/es8311/es8311.c: mclk_from_mclk_pin
 * = false path) rather than invented: that driver computes an "mclk_hz" for
 * its coefficient-table lookup equal to sample_rate * bits_per_sample * 2
 * channels in this mode, which is exactly the bit-clock frequency this
 * file's own PIO program produces (see SOUND_SYNTH_SAMPLE_RATE_HZ,
 * sound_synth.h), and
 * 32000 Hz / 16-bit / stereo lands exactly on one of that table's rows
 * (1,024,000 Hz, 32000 Hz - see ES8311_REG02..08 below), not a value
 * between rows. GPIO22 is left configured as a driven-low output (not a
 * floating input) since the codec's MCLK pin still physically exists on
 * this schematic even though nothing here reads it in this mode.
 *
 * SPEAKER AMP. GPIO19 drives the NS4150B's CTRL pin, pulled low by a 10k
 * resistor on the board (R10, "PA&SPEAKER&MIC" schematic block) - active
 * HIGH, defaults OFF. Driven high only after the I2S bus is already running
 * clean silence (see sound_init()'s ordering), so the amplifier's first
 * input is silence, not a power-on transient.
 *
 * WHY A SYNTHESISER, NOT A STORED SAMPLE BUFFER. A one-shot "play a sample by
 * id from flash" service (decision 0002 section 7's original phrasing) would
 * need to hold at least one phrase of PCM in RAM ahead of DMA - at this
 * design's own sample rate (SOUND_SYNTH_SAMPLE_RATE_HZ) and even a short ~0.7s stereo 16-bit
 * phrase, that is on the order of 90KB, more than decision 0002's entire
 * "headroom" budget (80KB, section "Memory") on its own, competing directly
 * with the 330KB framebuffer and 64KB app arena for the same 520KB of SRAM.
 * A sine-plus-envelope synthesiser costs a multiply and a couple of
 * transcendental calls per sample and zero flash/RAM for the waveform
 * itself: see sound_synth.c. The two small ping-pong buffers this file does
 * keep (SOUND_BUF_FRAMES each) exist only to give the DMA something to read
 * a few tens of milliseconds ahead of core0's poll loop, not to hold the
 * whole sound.
 *
 * THE SYNTHESISER ITSELF LIVES IN sound_synth.c/.h, not here, and that split
 * is load-bearing rather than tidiness: sound_synth.c has no PIO, DMA, i2c or
 * pico-sdk dependency, so it compiles unmodified into emu.wasm too
 * (emulator/wasm/build.ts), and the emulator's own audio (emu_abi.h's sound
 * section, emu_shim.c) calls the exact same sound_synth_alarm_sample()
 * function this file does. What a browser plays through WebAudio is
 * therefore genuinely this firmware's own synthesis, not a JavaScript
 * reimplementation of it - see emu_abi.h for the honest limits of that claim
 * (the timbre, not the tune).
 */
#include "sound.h"
#include "sound_synth.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"

#include "DEV_Config.h" // DEV_I2C_Write_Byte / DEV_I2C_Read_Byte, I2C_PORT=i2c1
#include "sound_i2s.pio.h"

/* ---- pins ---------------------------------------------------------------- */
#define PIN_I2S_DOUT 20 // DSDIN on the codec (speaker data, MCU -> codec)
#define PIN_I2S_DIN  21 // ASDOUT on the codec (mic data, codec -> MCU); unused
#define PIN_I2S_MCLK 22 // wired but not driven, see header comment
#define PIN_I2S_LRCK 23 // the pin missing from AGENTS.md's table
#define PIN_I2S_BCLK 24 // = PIN_I2S_LRCK + 1, required by the PIO program
#define PIN_PA_ENABLE 19 // NS4150B CTRL, active high

/* ---- ES8311 register map, only what this file writes --------------------
 *
 * Addresses and the bit-field meanings in the comments below are corroborated
 * against espressif/esp-bsp's es8311 driver (components/es8311/es8311.c and
 * es8311_reg.h, Apache-2.0, fetched 2026-08-13), a driver whose OTHER
 * register addresses and default I2C address (0x18) this file has already
 * independently confirmed against this board's own schematic (see the CE-pin
 * trace in this file's header comment) - the same "an address I can check
 * agrees, so I trust the ones I can't check either" reasoning sensors.c's
 * AXP2101 register comments already use for a PDF that would not extract as
 * text. The specific byte VALUES below are this file's own choice (a 32kHz,
 * 16-bit, BCLK-derived-clock configuration, not the reference driver's
 * default 48kHz/MCLK-pin one), computed from that driver's coefficient table
 * for the (mclk=1024000 Hz treated as the BCLK-derived reference, rate=32000
 * Hz) row - see the header comment above for why that specific row is what
 * this design's pin choices land on.
 */
#define ES8311_I2C_ADDR 0x18

#define ES8311_REG00_RESET   0x00 // reset / power-on / master-slave select (bit6, 0=slave)
#define ES8311_REG01_CLKMGR1 0x01 // clock enables (bits 0-5) + MCLK source select (bit7)
#define ES8311_REG02_CLKMGR2 0x02 // pre-divider / pre-multiplier
#define ES8311_REG03_CLKMGR3 0x03 // ADC oversample ratio + speed mode
#define ES8311_REG04_CLKMGR4 0x04 // DAC oversample ratio
#define ES8311_REG05_CLKMGR5 0x05 // ADC/DAC clock dividers
#define ES8311_REG06_CLKMGR6 0x06 // SCLK (BCLK) polarity + divider
#define ES8311_REG07_CLKMGR7 0x07 // LRCK divider, high bits
#define ES8311_REG08_CLKMGR8 0x08 // LRCK divider, low bits
#define ES8311_REG09_SDPIN   0x09 // DAC serial port (input) resolution
#define ES8311_REG0A_SDPOUT  0x0A // ADC serial port (output) resolution
#define ES8311_REG0D_SYS     0x0D // analog power-up
#define ES8311_REG0E_SYS     0x0E // PGA + ADC modulator enable
#define ES8311_REG12_SYS     0x12 // DAC power-up
#define ES8311_REG13_SYS     0x13 // headphone/speaker drive output enable
#define ES8311_REG1C_ADCEQ   0x1C // ADC equalizer bypass, DC offset cancel
#define ES8311_REG31_DACMUTE 0x31 // DAC mute: bits 6,5
#define ES8311_REG32_DACVOL  0x32 // DAC volume, 0-255 (0 = mute, 255 = 0dB)
#define ES8311_REG37_DACEQ   0x37 // DAC equalizer bypass / fade rate
#define ES8311_REGFD_CHIPID1 0xFD // chip ID, read-only, for a bus presence check

// 70/100 through the reference driver's own volume formula
// ((volume*256)/100)-1: chosen, not measured, as a moderate level that
// leaves headroom against a small speaker's own distortion (task brief: "a
// tiny speaker distorts easily, and distortion is what makes a sound
// harsh") - the synthesiser's own peak amplitude (see sound_synth.c)
// already keeps well under full scale too, so this is a second, independent
// margin, not the only one.
#define ES8311_VOLUME_PERCENT 70
#define ES8311_VOLUME_REG32   ((ES8311_VOLUME_PERCENT * 256 / 100) - 1)

static inline void es8311_write(uint8_t reg, uint8_t val) {
    DEV_I2C_Write_Byte(ES8311_I2C_ADDR, reg, val);
}

// Runs once, on core0, before sensors_start() launches core1 - see sound.h's
// header comment for why that makes this the one and only place this file
// ever issues an i2c1 transaction.
static void es8311_bring_up(void) {
    // Reset, then power-on. 0x80 also leaves bit6 (master/slave select) at
    // 0, i.e. slave mode - the reference driver does a separate read-modify-
    // write for that bit in es8311_fmt_config(), which is redundant here
    // since 0x80 already has bit6 clear; skipped rather than copied
    // pointlessly.
    es8311_write(ES8311_REG00_RESET, 0x1F);
    sleep_ms(20); // the reference driver's own reset-to-deassert delay
    es8311_write(ES8311_REG00_RESET, 0x00);
    es8311_write(ES8311_REG00_RESET, 0x80);

    // Clock manager: enable all internal clocks (bits 0-5 = 0x3F) and select
    // the BCLK/SCK pin as the source for the codec's internal master clock
    // (bit 7) instead of the MCLK pin - see this file's header comment for
    // why MCLK is not driven.
    es8311_write(ES8311_REG01_CLKMGR1, 0xBF);

    // Coefficient row (mclk=1,024,000 Hz, rate=32,000 Hz) from the reference
    // driver's coeff_div[] table: pre_div=1, pre_multi=3, adc_div=1,
    // dac_div=1, fs_mode=0 (single speed), lrck_h=0x00, lrck_l=0xFF,
    // bclk_div=4, adc_osr=0x10, dac_osr=0x10. Computed by hand from that
    // driver's own field formulas (es8311_sample_frequency_config()), not
    // guessed:
    es8311_write(ES8311_REG02_CLKMGR2, 0x18); // (pre_div-1)<<5 | pre_multi<<3 = 0<<5 | 3<<3
    es8311_write(ES8311_REG03_CLKMGR3, 0x10); // fs_mode<<6 | adc_osr = 0<<6 | 0x10
    es8311_write(ES8311_REG04_CLKMGR4, 0x10); // dac_osr
    es8311_write(ES8311_REG05_CLKMGR5, 0x00); // (adc_div-1)<<4 | (dac_div-1) = 0<<4 | 0
    es8311_write(ES8311_REG06_CLKMGR6, 0x03); // sclk not inverted (bit5=0) | (bclk_div-1)=3
    es8311_write(ES8311_REG07_CLKMGR7, 0x00); // lrck_h
    es8311_write(ES8311_REG08_CLKMGR8, 0xFF); // lrck_l

    // Format: 16-bit resolution on both the DAC input and ADC output serial
    // ports (reg |= 3<<2 for 16-bit, per the reference driver's resolution
    // table). The ADC/output side is configured for consistency with the
    // reference's own always-both sequence even though this design never
    // uses it (no mic capture) - deviating there is an untested combination
    // this design has no reason to risk.
    es8311_write(ES8311_REG09_SDPIN, 0x0C);
    es8311_write(ES8311_REG0A_SDPOUT, 0x0C);

    // Analog/DAC power-up sequence, verbatim from the reference driver's
    // es8311_init(): power up analog circuitry, enable PGA + ADC modulator,
    // power up the DAC, enable the speaker/headphone drive output, bypass
    // the ADC equalizer (cancel DC offset), bypass the DAC equalizer.
    es8311_write(ES8311_REG0D_SYS, 0x01);
    es8311_write(ES8311_REG0E_SYS, 0x02);
    es8311_write(ES8311_REG12_SYS, 0x00);
    es8311_write(ES8311_REG13_SYS, 0x10);
    es8311_write(ES8311_REG1C_ADCEQ, 0x6A);
    es8311_write(ES8311_REG37_DACEQ, 0x08);

    // Volume, then unmute. Set once, left alone forever after - see sound.h's
    // header comment for why playback never touches i2c1 again.
    es8311_write(ES8311_REG32_DACVOL, (uint8_t)ES8311_VOLUME_REG32);
    es8311_write(ES8311_REG31_DACMUTE, 0x00); // clear bits 6,5: unmuted

    // Bus presence check, not an identity assertion: this project's citation
    // discipline (AGENTS.md) is to label a guess as a guess, and the exact
    // expected chip-ID byte is not independently confirmed here, so this
    // logs the raw read rather than comparing it against a memorised value
    // and claiming a pass/fail it has not earned. A stuck bus (no ACK) would
    // still show up as an obviously wrong byte (0xFF from DEV_I2C_Read_Byte
    // reading a released, pulled-up line) to whoever reads the boot log.
    uint8_t chipId = DEV_I2C_Read_Byte(ES8311_I2C_ADDR, ES8311_REGFD_CHIPID1);
    printf("sound: ES8311 reg 0xFD (chip id) = 0x%02X\r\n", chipId);
}

/* ---- I2S over PIO + DMA ---------------------------------------------------
 *
 * PIO INSTANCE: NOT pio0. firmware/lib/QSPI_PIO/qspi_pio.c drives the panel
 * on pio0, state machines 0 and 1 (qspi.sm_4wire / qspi.sm_1wire) - but it
 * never calls pio_sm_claim() for either one, it just uses those indices
 * directly. That means the SDK's own claim bookkeeping for pio0 does not
 * know they are taken: asking pio_claim_unused_sm(pio0, ...) for a "free" SM
 * could hand back SM 0 or 1 and silently collide with the display, which
 * decision 0001/AGENTS.md's push-path gotchas already paid for once and
 * which this task's brief explicitly forbids disturbing. So this file never
 * touches pio0 at all, and claims from pio1 (falling back to pio2) instead -
 * through the SDK's claim API, not a hardcoded index, so if a future change
 * ever does add a real claim to pio0's other two state machines, or adds
 * something to pio1, this still finds a genuinely free one rather than
 * assuming.
 */
// SOUND_SYNTH_SAMPLE_RATE_HZ (32000, sound_synth.h) is both targets' sample
// rate; here it also has to be the BCLK-derived-clock coefficient-table row
// this file's ES8311 setup depends on - see i2s_hw_init()'s clkdiv comment.
#define SOUND_BUF_FRAMES 1024 // 32ms of audio per buffer at 32kHz - see
                              // sound_poll()'s header comment for why this
                              // needs to comfortably exceed gfx_push_all()'s
                              // worst-case ~12ms panel push.

static PIO s_pio;
static uint s_sm;
static uint s_pioOffset;
static int s_dmaChan = -1;
static bool s_hwReady = false;

static uint32_t s_bufA[SOUND_BUF_FRAMES];
static uint32_t s_bufB[SOUND_BUF_FRAMES];
static bool s_bufAIsPlaying; // which buffer the DMA channel is currently reading

static bool claim_pio_sm(PIO candidate) {
    int sm = pio_claim_unused_sm(candidate, false); // false: return -1, don't panic
    if (sm < 0) return false;
    s_pio = candidate;
    s_sm = (uint)sm;
    return true;
}

static void i2s_hw_init(void) {
    if (!claim_pio_sm(pio1) && !claim_pio_sm(pio2)) {
        // Nothing else in this firmware uses pio1 or pio2 (grepped, not
        // assumed), so this should be unreachable; if it ever fires, sound
        // stays silent rather than taking pio0's SMs on faith - see the
        // header comment above for why that specific shortcut is refused.
        printf("sound: FATAL: no free PIO state machine on pio1 or pio2, audio disabled\r\n");
        return;
    }

    s_pioOffset = pio_add_program(s_pio, &i2s_out_program);
    i2s_out_program_init(s_pio, s_sm, s_pioOffset, PIN_I2S_DOUT, PIN_I2S_LRCK);

    // SM clock: 64 SM-cycles per stereo frame (32 output bits x 2 SM-cycles
    // per side-set edge pair), so SM_freq = 64 * Fs. At Fs=32000 and this
    // board's 150MHz sys clock (DEV_Module_Init(), DEV_Config.c), that is
    // sys_clk / 2,048,000 = 73 + 62/256 exactly (150,000,000*256/2,048,000 =
    // 18750, and 18750 = 73*256 + 62 exactly) - a divider this clean is why
    // 32000 Hz was chosen over some other supported coefficient-table row,
    // not a coincidence: BCLK comes out at exactly 1.024MHz with zero
    // fractional error, which is also exactly the coefficient-table row this
    // file's ES8311 setup uses.
    pio_sm_set_clkdiv_int_frac(s_pio, s_sm, 73, 62);

    s_dmaChan = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(s_dmaChan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true)); // true: TX
    dma_channel_configure(s_dmaChan, &c, &s_pio->txf[s_sm], NULL, 0, false);

    memset(s_bufA, 0, sizeof(s_bufA));
    memset(s_bufB, 0, sizeof(s_bufB));

    pio_sm_set_enabled(s_pio, s_sm, true);
    // Kick off buffer A. The bus now runs forever, silence or chime - see
    // sound.h's header comment on why "stop" is a data-plane change only.
    dma_channel_transfer_from_buffer_now(s_dmaChan, s_bufA, SOUND_BUF_FRAMES);
    s_bufAIsPlaying = true;

    s_hwReady = true;
}

/* ---- play/stop/poll -------------------------------------------------------
 *
 * All three run on core0; there is no concurrency to guard here (no ISR, no
 * core1 involvement - see sound.h's header comment) beyond what a single
 * thread of execution already gives for free.
 */
static bool s_playing = false;
static uint32_t s_playSampleIndex = 0;
static uint32_t s_playStartMs = 0;

// Backstop only: the timer's own ALARM_MAX_MS (30s, timer.c) is the primary
// self-limit a child actually experiences. This is much longer and exists
// purely so a bug elsewhere that forgets to call sound_stop() cannot leave
// the speaker running until the battery dies - the same "backstop, not the
// mechanism" relationship sensors.c's OFFLEVEL raise has to the software
// power-off request (see its comment).
#define SOUND_SAFETY_CEILING_MS 120000u

static void refill_buffer(uint32_t *buf) {
    if (!s_playing) {
        memset(buf, 0, SOUND_BUF_FRAMES * sizeof(uint32_t));
        return;
    }
    if (to_ms_since_boot(get_absolute_time()) - s_playStartMs > SOUND_SAFETY_CEILING_MS) {
        s_playing = false;
        memset(buf, 0, SOUND_BUF_FRAMES * sizeof(uint32_t));
        return;
    }
    for (int i = 0; i < SOUND_BUF_FRAMES; i++) {
        float tSec = (float)s_playSampleIndex / (float)SOUND_SYNTH_SAMPLE_RATE_HZ;
        int16_t sample = sound_synth_alarm_sample(tSec);
        // Same sample in both I2S slots - see sound_i2s.pio's header comment
        // for why (mono codec, undocumented which slot it actually reads).
        buf[i] = ((uint32_t)(uint16_t)sample << 16) | (uint16_t)sample;
        s_playSampleIndex++;
    }
}

void sound_play(sound_id_t id) {
    (void)id; // one sound exists today; see sound.h's comment on why the enum stays
    if (!s_hwReady) return;
    s_playSampleIndex = 0;
    s_playStartMs = to_ms_since_boot(get_absolute_time());
    s_playing = true;
}

void sound_stop(void) {
    s_playing = false;
    if (!s_hwReady) return;
    // Zero BOTH buffers immediately, including whichever one the DMA is
    // currently mid-read of: this is not a data race worth guarding, since
    // overwriting the not-yet-read tail of an in-flight buffer with silence
    // is exactly what "immediate" is asking for, not a hazard to prevent.
    // The alternative (only changing what the NEXT refill synthesises) would
    // let up to one buffer's worth of already-queued chime (SOUND_BUF_FRAMES
    // / SOUND_SYNTH_SAMPLE_RATE_HZ, ~32ms) keep playing after dismissal - audible,
    // and not what "immediately" means for a child reaching for a beeping
    // object.
    memset(s_bufA, 0, sizeof(s_bufA));
    memset(s_bufB, 0, sizeof(s_bufB));
}

// Called once per core0 main-loop iteration (runtime.c), exactly like
// devlink_poll(). SOUND_BUF_FRAMES sizes the safety margin here: a buffer
// lasts 32ms, comfortably longer than gfx_push_all()'s worst-case ~12ms
// panel push (AGENTS.md's "~12ms full-panel push"), which is the only
// regularly-occurring stall on core0 while the alarm is ringing (every
// ALARM_FLASH_MS=250ms in timer.c's handle_alarm()) - so even a push landing
// right when a buffer finishes still leaves the OTHER, already-queued buffer
// enough headroom to keep the bus fed until this function runs again.
void sound_poll(void) {
    if (!s_hwReady) return;
    if (dma_channel_is_busy(s_dmaChan)) return; // still playing; nothing to do

    uint32_t *justFinished = s_bufAIsPlaying ? s_bufA : s_bufB;
    uint32_t *other = s_bufAIsPlaying ? s_bufB : s_bufA;

    // Retrigger the other buffer (already refilled by the previous call, or
    // still the init-time silence) before doing any synthesis work, so the
    // bus never waits on this function's own compute time.
    dma_channel_transfer_from_buffer_now(s_dmaChan, other, SOUND_BUF_FRAMES);
    s_bufAIsPlaying = !s_bufAIsPlaying;

    refill_buffer(justFinished);
}

/* ---- lifecycle ------------------------------------------------------------ */
void sound_init(void) {
    // MCLK pin: driven low, not read by the codec in this design (see the
    // header comment on the BCLK-derived clock mode) but held rather than
    // left floating.
    gpio_init(PIN_I2S_MCLK);
    gpio_set_dir(PIN_I2S_MCLK, GPIO_OUT);
    gpio_put(PIN_I2S_MCLK, 0);

    // Amp enable: default low (matches the board's own pull-down, R10, so
    // there is no discontinuity at the instant firmware takes over the pin),
    // driven high only after the bus is already running silence - see below.
    gpio_init(PIN_PA_ENABLE);
    gpio_set_dir(PIN_PA_ENABLE, GPIO_OUT);
    gpio_put(PIN_PA_ENABLE, 0);

    es8311_bring_up(); // the only i2c1 traffic this file ever issues
    i2s_hw_init();      // starts the bus running silence (both buffers zeroed)

    if (s_hwReady) {
        gpio_put(PIN_PA_ENABLE, 1); // now safe: the amp's input is silence, not a transient
    }

    printf("sound: init %s\r\n", s_hwReady ? "ok" : "FAILED (no PIO state machine)");
}
