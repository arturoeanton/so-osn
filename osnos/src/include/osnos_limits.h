#pragma once

#include "../micro/ipc.h"

#define OSNOS_PATH_MAX   128
#define OSNOS_NAME_MAX   64
#define OSNOS_INPUT_MAX  128

_Static_assert(
    OSNOS_PATH_MAX * 2 + 16 <= IPC_DATA_SIZE,
    "OSNOS_PATH_MAX too large for IPC payload: IPC_FS_COPY/MOVE pack src\\0dst"
);

_Static_assert(
    OSNOS_NAME_MAX <= OSNOS_PATH_MAX,
    "OSNOS_NAME_MAX must fit inside OSNOS_PATH_MAX"
);
