/*
 * A314 Emulation for FS-UAE
 *
 * Emulates the A314-CP (clockport variant) hardware interface
 * that allows communication between the Amiga and a virtual
 * Raspberry Pi environment.
 */

#ifndef UAE_A314_H
#define UAE_A314_H

#include "uae/types.h"

#ifdef __cplusplus
extern "C" {
#endif

void a314_init(void);
void a314_reset(void);
void a314_cleanup(void);
uae_u32 a314_bget(uaecptr addr);
void a314_bput(uaecptr addr, uae_u32 value);
int a314_is_enabled(void);
void a314_rethink(void);
void a314_hsync(void);

#ifdef __cplusplus
}
#endif

#endif /* UAE_A314_H */
