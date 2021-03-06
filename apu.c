#include "global.h"
#include "apu.h"
#include "system.h"

int duty_cycles[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1},
    {0, 0, 0, 0, 0, 0, 1, 1},
    {0, 0, 0, 0, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 0, 0},
};

int triangle_wave[32] = {
    15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1,  0,
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
};

void apu_envelope_update(APU_Envelope *envelope, u8 volume, bool loop) {
    if (envelope->start) {
        envelope->start = false;
        envelope->decay = 15;
        envelope->divider = volume;
    } else {
        if (envelope->divider == 0) {
            envelope->divider = volume;
            if (envelope->decay == 0) {
                if (loop)
                    envelope->decay = 15;
            } else {
                (envelope->decay)--;
            }
        } else {
            (envelope->divider)--;
        }
    }
}

void apu_quarter_frame(System *sys) {
    apu_envelope_update(&sys->pulse1_envelope, sys->pulse1_volume, sys->pulse1_halt);
    apu_envelope_update(&sys->pulse2_envelope, sys->pulse2_volume, sys->pulse2_halt);
    apu_envelope_update(&sys->noise_envelope, sys->noise_volume, sys->noise_halt);
    if (sys->triangle_reload)
        sys->triangle_counter = sys->triangle_counter_reload;
    else if (sys->triangle_counter > 0)
        sys->triangle_counter--;
    if (!sys->triangle_halt)
        sys->triangle_reload = false;
}

void apu_apply_sweep(APU_Sweep *sweep, u16 *period) {
    if (sweep->enable && sweep->divider == 0 && *period >= 8 && sweep->target < 0x800)
        *period = sweep->target;
    if (sweep->divider == 0 || sweep->reload) {
        sweep->divider = sweep->period;
        sweep->reload = false;
    } else {
        sweep->divider--;
    }
}

void apu_half_frame(System *sys) {
    if (!sys->pulse1_halt && sys->pulse1_length_counter > 0)
        sys->pulse1_length_counter--;
    apu_apply_sweep(&sys->pulse1_sweep, &sys->pulse1_period);
    if (!sys->pulse2_halt && sys->pulse2_length_counter > 0)
        sys->pulse2_length_counter--;
    apu_apply_sweep(&sys->pulse2_sweep, &sys->pulse2_period);
    if (!sys->triangle_halt && sys->triangle_length_counter > 0)
        sys->triangle_length_counter--;
    if (!sys->noise_halt && sys->noise_length_counter > 0)
        sys->noise_length_counter--;
}

void apu_calculate_sweep_target(APU_Sweep *sweep, u16 period, u16 i) {
    u16 change = period >> sweep->shift;
    if (sweep->negate)
        change = -change - i;
    sweep->target = period + change;
}

bool apu_timer_tick(u16 *timer, u16 period) {
    if (*timer == 0) {
        *timer = period;
        return true;
    } else {
        (*timer)--;
        return false;
    }
}

float apu_step(System *sys) {
    apu_calculate_sweep_target(&sys->pulse1_sweep, sys->pulse1_period, 1);
    float pulse1 = sys->pulse1_enable && sys->pulse1_length_counter > 0 && sys->pulse1_period >= 8 && sys->pulse1_sweep.target < 0x800
        ? (sys->pulse1_const_volume ? sys->pulse1_volume : sys->pulse1_envelope.decay)
            * duty_cycles[sys->pulse1_duty][sys->pulse1_sequencer]
        : 0;
    if ((sys->APU_cycle_counter & 1) == 0 && apu_timer_tick(&sys->pulse1_timer, sys->pulse1_period))
        sys->pulse1_sequencer = (sys->pulse1_sequencer - 1) & 7;

    apu_calculate_sweep_target(&sys->pulse2_sweep, sys->pulse2_period, 0);
    float pulse2 = sys->pulse2_enable && sys->pulse2_length_counter > 0 && sys->pulse2_period >= 8 && sys->pulse2_sweep.target < 0x800
        ? (sys->pulse2_const_volume ? sys->pulse2_volume : sys->pulse2_envelope.decay)
            * duty_cycles[sys->pulse2_duty][sys->pulse2_sequencer]
        : 0;
    if ((sys->APU_cycle_counter & 1) == 0 && apu_timer_tick(&sys->pulse2_timer, sys->pulse2_period))
        sys->pulse2_sequencer = (sys->pulse2_sequencer - 1) & 7;

    float triangle = sys->triangle_enable && sys->triangle_length_counter > 0
        ? triangle_wave[sys->triangle_sequencer]
        : 0;
    if (apu_timer_tick(&sys->triangle_timer, sys->triangle_period) && sys->triangle_length_counter > 0 && sys->triangle_counter > 0)
        sys->triangle_sequencer = (sys->triangle_sequencer + 1) % 32;

    float noise = sys->noise_enable && sys->noise_length_counter > 0 && !(sys->noise_lfsr & 0x0001)
        ? (sys->noise_const_volume ? sys->noise_volume : sys->noise_envelope.decay)
        : 0;
    if ((sys->APU_cycle_counter & 1) == 0 && apu_timer_tick(&sys->noise_timer, sys->noise_period)) {
        int feedback = (sys->noise_lfsr ^ sys->noise_lfsr >> (sys->noise_mode ? 6 : 1)) & 0x0001;
        sys->noise_lfsr = sys->noise_lfsr >> 1 | feedback << 14;
    }

    float dmc = sys->dmc_enable ? sys->dmc_output : 0;
    if ((sys->APU_cycle_counter & 1) == 0 && apu_timer_tick(&sys->dmc_timer, sys->dmc_period)) {
        if (!sys->dmc_silence) {
            if (sys->dmc_shift & 0x01) {
                if (sys->dmc_output <= 125)
                    sys->dmc_output += 2;
            } else {
                if (sys->dmc_output >= 2)
                    sys->dmc_output -= 2;
            }
        }
        sys->dmc_shift >>= 1;
        sys->dmc_shift_bits--;
        if (sys->dmc_shift_bits == 0) {
            sys->dmc_shift_bits = 8;
            sys->dmc_silence = sys->dmc_buffer_empty;
            if (!sys->dmc_buffer_empty) {
                sys->dmc_shift = sys->dmc_buffer;
                sys->dmc_buffer_empty = true;
            }
            if (sys->dmc_buffer_empty) {
                if (sys->dmc_length_counter > 0) {
                    sys->dmc_buffer = cpu_read(sys, sys->dmc_address);
                    sys->dmc_address = 0x8000 | (sys->dmc_address + 1);
                    sys->dmc_length_counter--;
                    if (sys->dmc_length_counter == 0 && sys->dmc_loop) {
                        sys->dmc_address = sys->dmc_sample_address;
                        sys->dmc_length_counter = sys->dmc_sample_length;
                    }
                    sys->dmc_buffer_empty = false;
                }
            }
        }
    }

    switch (sys->APU_sequencer_mode) {
    case 0:
        switch (sys->APU_cycle_counter) {
        case 0:
            break;
        case 7457:
            apu_quarter_frame(sys);
            break;
        case 14193:
            apu_quarter_frame(sys);
            apu_half_frame(sys);
            break;
        case 22371:
            apu_quarter_frame(sys);
            break;
        case 29828:
            break;
        case 29829:
            apu_quarter_frame(sys);
            apu_half_frame(sys);
            break;
        }
        break;
    case 1:
        switch (sys->APU_cycle_counter) {
        case 0:
            break;
        case 7457:
            apu_quarter_frame(sys);
            break;
        case 14193:
            apu_quarter_frame(sys);
            apu_half_frame(sys);
            break;
        case 22371:
            apu_quarter_frame(sys);
            break;
        case 37281:
            apu_quarter_frame(sys);
            apu_half_frame(sys);
            break;
        }
        break;
    }
    sys->APU_cycle_counter++;
    if (sys->APU_cycle_counter > (sys->APU_sequencer_mode ? 37281 : 29829))
        sys->APU_cycle_counter = 0;
    float pulse_out = pulse1 + pulse2 ? 95.88 / (8128 / (pulse1 + pulse2) + 100) : 0;
    float tnd_out = triangle || noise || dmc ? 159.79 / (1 / (triangle / 8227 + noise / 12241 + dmc / 22638) + 100) : 0;
    return pulse_out + tnd_out;
}
