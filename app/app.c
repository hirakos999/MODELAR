/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Hirakos */
/* MODELAR on fm1-base: the app's unity build (compiled into fm1-base's, after the platform). The sound is its own
 * -O2 unit (dsp/synth.c); the engine (clock, modulation, gestures, ARP, SEQ) runs at block rate before it; the
 * patches live in the flash (store.c); the panel and the screen are ui.c. */
#include "fastmath.h"
#include "engine.c"
#include "store.c"
#include "ui.c"
