/* Copyright (C) 2025 OnionHEN / LightningMods */

#include "kstuff_probe.h"

#include <onion/log.h>

#include <stddef.h>

int sceKernelMprotect(void *addr, size_t len, int prot);

bool kstuff_mprotect_available(void) {
  char probe[100];
  if (sceKernelMprotect(probe, sizeof(probe), 0x7) != 0)
    return false;

  if (sceKernelMprotect(probe, sizeof(probe), 0x3) != 0)
    LOG_ERROR("kstuff mprotect probe could not restore stack permissions");
  return true;
}
