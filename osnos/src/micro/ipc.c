#include "ipc.h"
#include "task.h"
#include "service.h"

static ipc_msg_t queue[IPC_QUEUE_SIZE];

static unsigned int read_index = 0;
static unsigned int write_index = 0;
static unsigned int count = 0;

void ipc_init(void) {
    read_index = 0;
    write_index = 0;
    count = 0;
}

osnos_status_t ipc_send(const ipc_msg_t *msg) {
    uint64_t pid = service_get_pid(msg->to);

    if (pid == 0) {
        return OSNOS_ESRCH;
    }

    if (count >= IPC_QUEUE_SIZE) {
        return OSNOS_EAGAIN;
    }

    queue[write_index] = *msg;
    /* Translate the service ID in `msg->to` into the actual receiver
     * pid in the queued copy. Receivers filter by their own pid
     * (sys_ipc_recv) so the queue MUST store pid, not the SID. The
     * sender's view of msg.to is unchanged. */
    queue[write_index].to = pid;

    write_index++;
    write_index %= IPC_QUEUE_SIZE;

    count++;

    task_unblock(pid);

    return OSNOS_OK;
}

size_t ipc_pending(void) {
    return count;
}

bool ipc_recv(uint64_t to, ipc_msg_t *out) {
    if (count == 0) {
        return false;
    }

    for (unsigned int i = 0; i < count; i++) {

        unsigned int index =
            (read_index + i) % IPC_QUEUE_SIZE;

        if (queue[index].to == to) {

            *out = queue[index];

            for (unsigned int j = i;
                 j + 1 < count;
                 j++) {

                unsigned int from =
                    (read_index + j + 1)
                    % IPC_QUEUE_SIZE;

                unsigned int dest =
                    (read_index + j)
                    % IPC_QUEUE_SIZE;

                queue[dest] = queue[from];
            }

            write_index =
                (write_index + IPC_QUEUE_SIZE - 1)
                % IPC_QUEUE_SIZE;

            count--;

            return true;
        }
    }

    return false;
}

bool ipc_recv_block(
    uint64_t to,
    ipc_msg_t *out
) {
    if (ipc_recv(to, out)) {
        return true;
    }

    task_t *task = task_current();

    if (task) {
        task->state = TASK_BLOCKED;
    }

    return false;
}
