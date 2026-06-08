#ifndef INTRO_GAS_STATION_H
#define INTRO_GAS_STATION_H

#include "core/types.h"

/* Run the gas station scene.
   Returns non-zero if user pressed a button to skip.
   Ported from GAS_STATION in asm/intro/gas_station.asm */
uint16_t gas_station(void);

/* Yield-free one-shot setup for the gas-station scene (INIT_ENTITY_SYSTEM +
   GAS_STATION_LOAD). Run before pushing GAME_MODE_GAS_STATION; the gas_station()
   wrapper calls it too. */
void gas_station_setup(void);

#endif /* INTRO_GAS_STATION_H */
