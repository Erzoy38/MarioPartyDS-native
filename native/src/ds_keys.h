// DS key bits, in the order the hardware uses (KEYINPUT, and X and Y from the ARM7's EXTKEYIN).
#pragma once

#include <SDL3/SDL_stdinc.h>

enum DsKey : Uint32 {
    kA = 1, kB = 2, kSelect = 4, kStart = 8, kRight = 0x10, kLeft = 0x20, kUp = 0x40, kDown = 0x80,
    kR = 0x100, kL = 0x200, kX = 0x400, kY = 0x800,
};
