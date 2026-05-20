#include "service.h"

#define MAX_SERVICES 16

static uint64_t service_pid[MAX_SERVICES];

void service_register(uint64_t service_id, uint64_t pid) {
    if (service_id < MAX_SERVICES) {
        service_pid[service_id] = pid;
    }
}

uint64_t service_get_pid(uint64_t service_id) {
    if (service_id < MAX_SERVICES) {
        return service_pid[service_id];
    }

    return 0;
}
