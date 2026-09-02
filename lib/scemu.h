/*
 * scemu — Roland Sound Canvas emulator library
 *
 * One instance is one machine.  Nothing here is thread-safe: a host drives an
 * instance from one thread, normally the audio thread, and feeds it MIDI from
 * a queue it drains before each render call.  Time is counted in frames of
 * the machine's own sample clock; all offsets are relative to the start of
 * the next render call.
 */
#ifndef SCEMU_H
#define SCEMU_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct scemu scemu_t;

typedef enum scemu_model
{
	SCEMU_MODEL_SC88,
	SCEMU_MODEL_SC88VL,
	SCEMU_MODEL_SC88PRO,
	SCEMU_MODEL_VEGSPRO,
	SCEMU_MODEL_COUNT
} scemu_model_t;

#define SCEMU_MAX_WAVE_ROMS 8

/* ROM images exactly as dumped from the chips; the library knows each model's
 * byte order and wave ROM wiring.  program_rom is the control ROM.  Wave ROMs
 * are given in board order (the SC-88's IC14, IC8, IC7, IC6). */
typedef struct scemu_roms
{
	const void *program_rom;
	size_t program_rom_size;
	const void *wave_rom[SCEMU_MAX_WAVE_ROMS];
	size_t wave_rom_size[SCEMU_MAX_WAVE_ROMS];
	int wave_rom_count;
} scemu_roms_t;

/* Executable memory for the DSP code generators.  Both hooks NULL selects the
 * library's own allocator (the build must then keep sljit's allocator in).
 * The returned block must be writable and executable. */
typedef struct scemu_config
{
	void *(*exec_alloc)(size_t size, void *user);
	void (*exec_free)(void *block, void *user);
	void *user;
} scemu_config_t;

/* Front panel buttons.  The matrix positions are the same on the SC-88 and the
 * SC-88Pro; the names follow the Pro's panel, with the SC-88 reading of a
 * shared position in the comment. */
typedef enum scemu_button
{
	SCEMU_BUTTON_ALL,
	SCEMU_BUTTON_MUTE,
	SCEMU_BUTTON_SC55_MAP,
	SCEMU_BUTTON_SC88_MAP,        /* EQ on the SC-88 */
	SCEMU_BUTTON_PREVIEW,         /* the volume knob's push switch */
	SCEMU_BUTTON_PART_LEFT,
	SCEMU_BUTTON_PART_RIGHT,
	SCEMU_BUTTON_INSTRUMENT_LEFT,
	SCEMU_BUTTON_INSTRUMENT_RIGHT,
	SCEMU_BUTTON_LEVEL_LEFT,
	SCEMU_BUTTON_LEVEL_RIGHT,
	SCEMU_BUTTON_PAN_LEFT,
	SCEMU_BUTTON_PAN_RIGHT,
	SCEMU_BUTTON_REVERB_LEFT,
	SCEMU_BUTTON_REVERB_RIGHT,
	SCEMU_BUTTON_CHORUS_LEFT,
	SCEMU_BUTTON_CHORUS_RIGHT,
	SCEMU_BUTTON_KEY_SHIFT_LEFT,  /* Key Shift / Delay */
	SCEMU_BUTTON_KEY_SHIFT_RIGHT,
	SCEMU_BUTTON_MIDI_CH_LEFT,
	SCEMU_BUTTON_MIDI_CH_RIGHT,
	SCEMU_BUTTON_USER_INST,       /* User Inst / EFX */
	SCEMU_BUTTON_SELECT,          /* Select / EFX On/Off */
	SCEMU_BUTTON_EDIT1_LEFT,      /* Vib Rate / Attack / EFX Type */
	SCEMU_BUTTON_EDIT1_RIGHT,
	SCEMU_BUTTON_EDIT2_LEFT,      /* Vib Depth / Cutoff / Decay / EFX Param */
	SCEMU_BUTTON_EDIT2_RIGHT,
	SCEMU_BUTTON_EDIT3_LEFT,      /* Vib Delay / Resonance / Release / EFX Value */
	SCEMU_BUTTON_EDIT3_RIGHT,
	SCEMU_BUTTON_COUNT
} scemu_button_t;

/* Rear panel computer switch, read by the firmware through a resistor ladder. */
typedef enum scemu_computer_switch
{
	SCEMU_COMPUTER_MIDI,
	SCEMU_COMPUTER_PC1,
	SCEMU_COMPUTER_PC2,
	SCEMU_COMPUTER_MAC
} scemu_computer_switch_t;

/* LED bit positions in the mask returned by scemu_leds(). */
typedef enum scemu_led
{
	SCEMU_LED_ALL,
	SCEMU_LED_MUTE,
	SCEMU_LED_SC55_MAP,
	SCEMU_LED_SC88_MAP,           /* EQ on the SC-88 */
	SCEMU_LED_EDIT1,              /* the three triangles beside the edit rows */
	SCEMU_LED_EDIT2,
	SCEMU_LED_EDIT3,
	SCEMU_LED_USER_INST,          /* the lens; green */
	SCEMU_LED_USER_INST_RED,      /* the Pro's second lens die; both lit is EFX mode */
	SCEMU_LED_COUNT
} scemu_led_t;

/* The LCD controller's memory.  The glass is a custom segment layout driven by
 * an HD44780-compatible controller: the 16 part bars, the L/R marks and the
 * numeric and text fields are all character cells; the layout is in
 * docs/design.md.  The host draws the glass from this. */
typedef struct scemu_lcd
{
	uint8_t ddram[80];
	uint8_t cgram[64];
	bool display_on;
	bool changed;
} scemu_lcd_t;

/* MIDI IN ports.  The Pro's front panel jack is a switch onto port B. */
enum { SCEMU_MIDI_IN_A = 0, SCEMU_MIDI_IN_B = 1 };

/* Output pairs.  The SC-88 has one; the Pro adds OUTPUT 2. */
enum { SCEMU_OUTPUT_1 = 0, SCEMU_OUTPUT_2 = 1 };

typedef void (*scemu_midi_out_fn)(const uint8_t *bytes, size_t count, void *user);

/* Lifetime.  scemu_create validates the ROM set for the model and returns NULL
 * if it does not fit; scemu_error(NULL) then says why. */
scemu_t *scemu_create(scemu_model_t model, const scemu_roms_t *roms, const scemu_config_t *config);
void scemu_destroy(scemu_t *m);
const char *scemu_error(const scemu_t *m);
scemu_model_t scemu_model(const scemu_t *m);

/* The machine's own sample rate in Hz, 24.576 MHz / 768.  The host resamples. */
uint32_t scemu_sample_rate(const scemu_t *m);
int scemu_output_count(const scemu_t *m);

/* Power-on reset, then optionally run the firmware until it is idle at the
 * front panel (the boot animation done, the tone generator initialised).
 * scemu_boot runs the machine as fast as the host allows and returns the
 * number of frames it took; a host that wants instant instantiation saves
 * the state afterwards and loads it into later instances. */
void scemu_reset(scemu_t *m);
uint64_t scemu_boot(scemu_t *m);

/* Render `frames` frames.  out[pair] points at interleaved stereo, one 32-bit
 * word per channel holding the DAC's 24-bit sample, sign extended.  An out
 * pointer may be NULL.  MIDI queued with an offset inside the range is
 * delivered on its frame. */
void scemu_render(scemu_t *m, int32_t *const out[2], size_t frames);

/* MIDI.  Bytes are raw wire bytes; running status is fine; a message may be
 * split across calls.  frame_offset is measured from the start of the next
 * render call. */
void scemu_midi_write(scemu_t *m, int port, const uint8_t *bytes, size_t count, uint32_t frame_offset);
void scemu_set_midi_out(scemu_t *m, scemu_midi_out_fn fn, void *user);

/* Panel. */
void scemu_button(scemu_t *m, scemu_button_t button, bool down);
void scemu_set_computer_switch(scemu_t *m, scemu_computer_switch_t sw);
uint32_t scemu_leds(const scemu_t *m);
const scemu_lcd_t *scemu_lcd(scemu_t *m);
void scemu_lcd_ack(scemu_t *m);

/* State.  A state is the whole machine at a frame boundary, including the
 * battery-backed SRAM, and is only valid for the same model and ROM set.
 * scemu_state_load returns false and leaves the machine unchanged if the
 * buffer does not fit. */
size_t scemu_state_size(const scemu_t *m);
size_t scemu_state_save(const scemu_t *m, void *buffer, size_t size);
bool scemu_state_load(scemu_t *m, const void *buffer, size_t size);

/* The battery-backed SRAM on its own, for a host that keeps user settings
 * across sessions without a full state. */
size_t scemu_nvram_size(const scemu_t *m);
size_t scemu_nvram_get(const scemu_t *m, void *buffer, size_t size);
bool scemu_nvram_set(scemu_t *m, const void *buffer, size_t size);

#ifdef __cplusplus
}
#endif

#endif
