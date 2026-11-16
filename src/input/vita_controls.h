// The controls page of the PS Vita's pad: the names of its inputs, and the wait
// for the next input. No pad here: input_vita.cpp gives the samples.
#ifndef OPENCMR2_VITA_CONTROLS_H
#define OPENCMR2_VITA_CONTROLS_H

#include "input/input_internal.h"

#include <stdint.h>

struct VitaPad {
    bool buttons[VITA_BUTTON_COUNT];
    int16_t axes[VITA_AXIS_COUNT];     // -32768..32767, as SDL's
};

// The input's name on the controls page, in plain words: the game's fonts have
// no PlayStation symbols.
const char *VitaControls_Name(const Source &source);

// A button that is down, or a stick half moved half way from where it was
// when the capture began.
bool VitaCapture_FindInput(const VitaPad &pad, const int16_t base[VITA_AXIS_COUNT], Source *out);

class VitaCapture {
public:
    void Begin(int action, const VitaPad &pad, uint32_t nowMs);
    // 1 with the input in *out, -1 when given up (ten seconds), 0 while it waits.
    int Update(const VitaPad &pad, uint32_t nowMs, Source *out);
    int Action() const { return _action; }

private:
    bool _active = false;
    bool _released = false;     // the input that opened the capture was let go
    int _action = 0;
    uint32_t _startMs = 0;
    int16_t _base[VITA_AXIS_COUNT] = {};
};

#endif
