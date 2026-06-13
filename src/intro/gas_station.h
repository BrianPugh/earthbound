#ifndef INTRO_GAS_STATION_H
#define INTRO_GAS_STATION_H

#include "core/types.h"

/* Yield-free one-shot setup for the gas-station scene (INIT_ENTITY_SYSTEM +
   GAS_STATION_LOAD). Run before pushing GAME_MODE_GAS_STATION. (The blocking
   gas_station() pump bridge — port of GAS_STATION in asm/intro/gas_station.asm —
   was deleted in D4b; init_intro STEP_PUSHes the mode.) */
void gas_station_setup(void);

#endif /* INTRO_GAS_STATION_H */
