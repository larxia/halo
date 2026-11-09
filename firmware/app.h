/* SPDX-License-Identifier: MPL-2.0
 * HALO — an ambient workstation for the M-VAVE FM-1, on fm1-base. This header is the app's half of the contract
 * with the platform (base/device/unity.c, base/host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "HALO"
#define APP_SPLASH_RGB RGB(247, 170, 200)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz */
#define APP_TOUCH                        /* velocity from the keys (lib/touch.h) */
#define APP_OVERLOAD                     /* the render near its limit: lighter engines (app_overload) */
#define APP_SPLASH                       /* our own boot screen: the halo and its name */
void app_overload(void);
void app_splash(void);

/* the store: settings, the sound (every parameter), the song (patterns), and the panel calibration (the platform's) */
enum { OBJ_SETTINGS, OBJ_PANEL, OBJ_SOUND, OBJ_SONG, OBJ_NOBJ };
#define APP_STORE_MAP {0xFC000u, 0xDC000u, 0xD8000u, 0xDA000u}

/* backup and restore over USB (fm1.toml backup = true; base/web/backup, base/tools/fm1_backup.py): all or nothing */
#define APP_BACKUP_OBJS {OBJ_SETTINGS, OBJ_SOUND, OBJ_SONG}
#define APP_BACKUP_MAGIC 0x424F4C48u           /* "HLOB" */
#define APP_BACKUP_VERSION 1u
#define APP_BACKUP_WHAT "replaces sound and song"
int app_backup_playing(void);
void app_backup_flush(void);
void app_backup_hold(int on);
void app_backup_restored(void);

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

/* the host simulator's checks (host/plat_host.c "expect") */
#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
