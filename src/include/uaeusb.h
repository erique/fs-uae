/*
 * UAE - The Un*x Amiga Emulator
 *
 * uaeusb.device — Poseidon USB host controller backed by host libusb
 */

#ifndef UAE_UAEUSB_H
#define UAE_UAEUSB_H

#include "uae/types.h"

uaecptr UaeusbStartup (uaecptr resaddr);
void UaeusbInstall (void);
void UaeusbReset (void);
void UaeusbFree (void);

#endif /* UAE_UAEUSB_H */
