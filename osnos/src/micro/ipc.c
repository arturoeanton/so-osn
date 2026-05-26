#include "ipc.h"
#include "task.h"
#include "service.h"

/* Forward decl — ipc_recv calls task_wake_pollers when it frees a
 * queue slot so any sender blocked in sys_ipc_send (queue-full case)
 * gets to retry. Defined in task.c. */
void task_wake_pollers(void);

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
    /* Two-step routing:
     *   1. Try service-ID lookup. This covers the canonical case
     *      (clients address kernel servers via SERVER_FOO constants).
     *   2. If no service matches, fall through to direct-pid routing
     *      so a server (e.g. oxsrv) can deliver events back to a
     *      specific client. The pid must correspond to a live task.
     *
     * Both code paths preserve the invariant that the queue stores
     * receiver pid (never SID) so sys_ipc_recv can match by pid.
     */
    uint64_t pid = service_get_pid(msg->to);
    if (pid == 0) {
        /* Direct-pid routing fallback (FASE 12 / Ox client events). */
        if (msg->to == 0) return OSNOS_ESRCH;
        task_t *target = task_by_pid(msg->to);
        if (!target) return OSNOS_ESRCH;
        /* Reject ZOMBIE/DEAD targets — they can never receive again.
         * Without this, oxsrv events sent to a just-died client get
         * queued and orphan the slot until reaper-side ipc_drop_for_pid
         * catches them. */
        if (target->state == TASK_ZOMBIE ||
            target->state == TASK_DEAD   ||
            target->pml4 == 0) {
            return OSNOS_ESRCH;
        }
        pid = msg->to;
    }

    if (count >= IPC_QUEUE_SIZE) {
        return OSNOS_EAGAIN;
    }

    queue[write_index] = *msg;
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

bool ipc_has_for_pid(uint64_t pid) {
    if (pid == 0) return false;
    for (unsigned int i = 0; i < count; i++) {
        unsigned int idx = (read_index + i) % IPC_QUEUE_SIZE;
        if (queue[idx].to == pid) return true;
    }
    return false;
}

void ipc_drop_for_pid(uint64_t pid) {
    if (pid == 0 || count == 0) return;
    /* Walk the logical queue (read_index .. read_index+count) and
     * rebuild it in place, skipping entries that target the dying
     * pid. Same pattern as ipc_recv's compaction but we drop ALL
     * matches in one pass. */
    unsigned int new_count = 0;
    unsigned int w = read_index;
    for (unsigned int i = 0; i < count; i++) {
        unsigned int r = (read_index + i) % IPC_QUEUE_SIZE;
        if (queue[r].to == pid) continue;          /* drop */
        if (r != w) queue[w] = queue[r];
        w = (w + 1) % IPC_QUEUE_SIZE;
        new_count++;
    }
    write_index = w;
    count = new_count;
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

            /* Freed a slot — wake any sender blocked in sys_ipc_send
             * waiting for queue space. Cheap (16 task slots checked). */
            task_wake_pollers();
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
