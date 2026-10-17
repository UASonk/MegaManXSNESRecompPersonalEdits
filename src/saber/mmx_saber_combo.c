#include "mmx_saber_combo.h"

#include <string.h>
#include "mmx_saber_attack.h"
#include "mmx_saber_state.h"

#include "../mmx_zero.h"
#include "mmx_saber_sfx.h"

typedef struct MmxSaberComboState {
  uint8_t window_ticks;
  uint8_t window_frames;
  uint8_t pre_live_mask;
  uint8_t pre_burst;
  uint16_t reserved_slot;
  unsigned finisher_cue_count;
  bool pre_valid;
  bool slash_request;
  bool slash_request_sent;
  /* Appended for savestate-prefix compatibility. */
  MmxSaberPadKind last_attack_kind;
  MmxSaberPadPhase last_attack_phase;
} MmxSaberComboState;

static MmxSaberComboState state = {
  0, MMX_SABER_DEFAULT_FINISHER_WINDOW, 0, 0, 0, 0, false, false, false,
  SABER_KIND_NONE, SABER_PHASE_IDLE};
static uint8_t *runtime_ram;

static unsigned word(const uint8_t *p) {
  return p[0] | ((unsigned)p[1] << 8);
}

static uint8_t live_burst_mask(const uint8_t *ram, MmxZeroState snapshot) {
  uint8_t live = 0;
  if (!ram) return 0;
  for (unsigned i = 0; i < 8; ++i) {
    const unsigned d = 0x1228 + i * 64;
    if ((snapshot.shot_mask & (uint8_t)(1u << i)) && ram[d] &&
        ram[d + 10] == 3 && word(ram + d + 0x3e) != 0x5a53)
      live |= (uint8_t)(1u << i);
  }
  return live;
}

static void release_reservation(uint8_t *ram) {
  uint8_t *target = ram ? ram : runtime_ram;
  const unsigned d = state.reserved_slot;
  if (target && d)
    (void)MmxSaberWaveRuntimeReleaseReservation(target, d);
  state.reserved_slot = 0;
}

static bool reserve_wave_slot(uint8_t *ram) {
  unsigned first = 0;
  if (!MmxSaberWaveRuntimeReserve(ram, &first)) return false;
  state.reserved_slot = (uint16_t)first;
  return true;
}

void MmxSaberComboSetWindowFrames(unsigned frames) {
  if (frames > MMX_SABER_MAX_FINISHER_WINDOW)
    frames = MMX_SABER_MAX_FINISHER_WINDOW;
  state.window_frames = (uint8_t)frames;
}

void MmxSaberComboReset(uint8_t *ram) {
  if (ram) runtime_ram = ram;
  release_reservation(ram);
  state.window_ticks = 0;
  state.pre_live_mask = 0;
  state.pre_burst = 0;
  state.pre_valid = false;
  state.slash_request = false;
  state.slash_request_sent = false;
  state.last_attack_kind = SABER_KIND_NONE;
  state.last_attack_phase = SABER_PHASE_IDLE;
  state.finisher_cue_count = 0;
}

void MmxSaberComboCancel(uint8_t *ram) {
  MmxSaberComboReset(ram);
}

bool MmxSaberComboPrePlayer(uint8_t *ram, bool saber_pressed) {
  const MmxZeroState snapshot = MmxZeroGetState();
  const MmxSaberAttackSnapshot attack = MmxSaberAttackSnapshotGet();
  bool claimed = false;
  bool opened_now = false;
  if (ram) runtime_ram = ram;

  /* With the buster disabled, the third ground slash becomes the gateway to
   * the existing X3 finisher/wave move. Once Ground3 naturally completes,
   * pressing Y during this short window requests the finisher instead of
   * starting another ordinary swing. */
  if (attack.phase == SABER_PHASE_IDLE) {
    if (ram && state.last_attack_kind == SABER_KIND_GROUND3 &&
        state.last_attack_phase != SABER_PHASE_IDLE &&
        ram[0xd1] == 2 && ram[0xd2] == 4 && ram[0xba9] == 2 &&
        (ram[0xbcf] & 127) && !ram[0x1f0c] && !ram[0x1f23] &&
        !ram[0x1f48] && ram[0x0baa] <= 0x0a &&
        !snapshot.slash && !state.window_ticks && state.window_frames) {
      state.window_ticks = state.window_frames;
      opened_now = true;
    }
    state.last_attack_kind = SABER_KIND_NONE;
    state.last_attack_phase = SABER_PHASE_IDLE;
  } else {
    state.last_attack_kind = attack.kind;
    state.last_attack_phase = attack.phase;
  }

  state.pre_live_mask = live_burst_mask(ram, snapshot);
  state.pre_burst = snapshot.burst;
  state.pre_valid = true;

  if (state.window_ticks && saber_pressed && !snapshot.slash) {
    /* The edge belongs to this window even when allocation cannot succeed. */
    claimed = true;
    state.window_ticks = 0;
    /* Saber Zero's third slash is the end of the combo, not a wave-projectile
     * launcher. Consume the input edge and close the window without reserving
     * or spawning the legacy finisher projectile. */
  }
  if (state.window_ticks && !opened_now) --state.window_ticks;
  return claimed;
}

bool MmxSaberComboLegacySlashRequest(const uint8_t *ram) {
  if (!ram || !state.slash_request) return false;
  runtime_ram = (uint8_t *)(uintptr_t)ram;
  state.slash_request = false;
  state.slash_request_sent = true;
  return true;
}

void MmxSaberComboPlayerEnd(uint8_t *ram) {
  const MmxZeroState snapshot = MmxZeroGetState();
  const uint8_t post_live_mask = live_burst_mask(ram, snapshot);
  if (ram) runtime_ram = ram;

  /* A burst-fired flag is not enough: only a newly live class-3 slot opens
   * the window, and it must be the second X3 burst. */
  if (state.pre_valid && state.pre_burst == 2 &&
      (post_live_mask & (uint8_t)~state.pre_live_mask) &&
      !snapshot.slash && !state.window_ticks && state.window_frames)
    state.window_ticks = state.window_frames;
  state.pre_valid = false;

  /* Old src/mmx_zero.c advances the legacy finisher to age 7 and publishes
   * the reserved wave in that same update.  The current bridge observes the
   * equivalent post-native player-end seam before the weapon pass. */
  if (snapshot.slash == 7 && state.reserved_slot) {
    (void)MmxSaberWaveRuntimePublish(ram, state.reserved_slot);
    state.reserved_slot = 0;
  }

  if (state.slash_request_sent) {
    if (snapshot.slash == 1) {
      MmxSaberSfxPlayForAttack(MMX_SABER_SFX_ATTACK_X3_FINISHER);
      ++state.finisher_cue_count;
      state.slash_request_sent = false;
    } else if (!snapshot.slash) {
      /* The request was consumed, but upstream did not publish slash == 1. */
      state.slash_request_sent = false;
      release_reservation(ram);
    }
  } else if (state.slash_request) {
    /* The player tick was skipped or could not poll the seam this frame. */
    state.slash_request = false;
    release_reservation(ram);
  }
}

unsigned MmxSaberComboWindowTicks(void) {
  return state.window_ticks;
}

unsigned MmxSaberComboReservedSlot(void) {
  return state.reserved_slot;
}

unsigned MmxSaberComboFinisherCueCount(void) {
  return state.finisher_cue_count;
}

size_t MmxSaberComboStateSize(void) {
  return sizeof(state);
}

void MmxSaberComboStateSave(uint8_t *out) {
  memcpy(out, &state, sizeof(state));
}

void MmxSaberComboStateLoad(const uint8_t *in) {
  /* The finisher window length is tuning, not state. */
  const uint8_t window_frames = state.window_frames;
  memcpy(&state, in, sizeof(state));
  state.window_frames = window_frames;
}
