/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Hirakos */
/* MODELAR: a semi-modular drone synth played like a Kaoss Pad (README.md).
 * This header is the app's half of the contract with fm1-base (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "synth.h"
#include "md.h"

#define APP_NAME "MODELAR"
#define APP_SPLASH_RGB RGB(255, 46, 136)
#define APP_SPLASH                       /* the logo: ui.c app_splash */
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#ifdef APP_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define MD_VERSION APP_VERSION
#else
#define MD_VERSION "DEV"
#endif

/* the store: the system settings and the panel calibration (the platform's); the patches are in the data area (store.c) */
enum { OBJ_SYS, OBJ_PANEL, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);
void app_splash(void);

/* the host simulator's checks (host/plat_host.c "expect"): see ui.c app_expect */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
