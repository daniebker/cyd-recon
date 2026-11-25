// Cyberpunk UI: boot sequence, device list, target tracker, RF spectrum.
#pragma once
#include <Arduino.h>
#include "touch.h"

namespace ui {

bool begin();  // allocate frame buffer
// Boot console: append a line and render it immediately
void bootLine(const char* text, int status);  // status: 0 none, 1 OK, -1 FAIL
void bootDone();
// Called by main for each observation (for the event log ticker)
void onNewDevice(int idx);
// Device slot currently being tracked (-1 if none)
int trackedIndex();
// Render + handle input; call every loop
void frame(const touch::State& t, bool backButton);

}  // namespace ui
