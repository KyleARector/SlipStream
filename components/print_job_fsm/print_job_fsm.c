#include "print_job_fsm.h"

#include <string.h>

static print_job_state_t next_state(print_job_state_t state, print_job_event_t event, bool *ok)
{
    switch (state) {
    case PRINT_JOB_STATE_IDLE:
        if (event == PRINT_JOB_EVENT_START) {
            *ok = true;
            return PRINT_JOB_STATE_FORMATTING;
        }
        break;

    case PRINT_JOB_STATE_FORMATTING:
        if (event == PRINT_JOB_EVENT_FORMATTED) {
            *ok = true;
            return PRINT_JOB_STATE_SENDING;
        }
        if (event == PRINT_JOB_EVENT_ERROR) {
            *ok = true;
            return PRINT_JOB_STATE_ERROR;
        }
        break;

    case PRINT_JOB_STATE_SENDING:
        if (event == PRINT_JOB_EVENT_SENT) {
            *ok = true;
            return PRINT_JOB_STATE_PRINTING;
        }
        if (event == PRINT_JOB_EVENT_ERROR) {
            *ok = true;
            return PRINT_JOB_STATE_ERROR;
        }
        break;

    case PRINT_JOB_STATE_PRINTING:
        if (event == PRINT_JOB_EVENT_PRINTED) {
            *ok = true;
            return PRINT_JOB_STATE_COMPLETE;
        }
        if (event == PRINT_JOB_EVENT_ERROR) {
            *ok = true;
            return PRINT_JOB_STATE_ERROR;
        }
        break;

    case PRINT_JOB_STATE_COMPLETE:
    case PRINT_JOB_STATE_ERROR:
        if (event == PRINT_JOB_EVENT_RESET) {
            *ok = true;
            return PRINT_JOB_STATE_IDLE;
        }
        break;
    }

    *ok = false;
    return state;
}

void print_job_fsm_init(print_job_fsm_t *fsm)
{
    if (fsm == NULL) {
        return;
    }
    fsm->state = PRINT_JOB_STATE_IDLE;
}

print_job_state_t print_job_fsm_get_state(const print_job_fsm_t *fsm)
{
    if (fsm == NULL) {
        return PRINT_JOB_STATE_IDLE;
    }
    return fsm->state;
}

bool print_job_fsm_handle_event(print_job_fsm_t *fsm, print_job_event_t event)
{
    if (fsm == NULL) {
        return false;
    }

    bool ok = false;
    print_job_state_t new_state = next_state(fsm->state, event, &ok);
    if (ok) {
        fsm->state = new_state;
    }
    return ok;
}

void print_job_queue_init(print_job_queue_t *queue)
{
    if (queue == NULL) {
        return;
    }
    queue->head = 0;
    queue->count = 0;
}

bool print_job_queue_is_empty(const print_job_queue_t *queue)
{
    return queue == NULL || queue->count == 0;
}

bool print_job_queue_is_full(const print_job_queue_t *queue)
{
    return queue != NULL && queue->count == PRINT_JOB_QUEUE_CAPACITY;
}

size_t print_job_queue_count(const print_job_queue_t *queue)
{
    return queue == NULL ? 0 : queue->count;
}

bool print_job_queue_push(print_job_queue_t *queue, print_job_type_t type, const char *payload, size_t payload_len,
                           bool cut_after)
{
    if (queue == NULL || payload == NULL) {
        return false;
    }
    if (print_job_queue_is_full(queue)) {
        return false;
    }
    if (payload_len >= PRINT_JOB_TEXT_MAX_LEN) {
        return false;
    }

    size_t tail = (queue->head + queue->count) % PRINT_JOB_QUEUE_CAPACITY;
    queue->jobs[tail].type = type;
    memcpy(queue->jobs[tail].payload, payload, payload_len);
    queue->jobs[tail].payload[payload_len] = '\0';
    queue->jobs[tail].payload_len = payload_len;
    queue->jobs[tail].cut_after = cut_after;
    queue->count++;
    return true;
}

bool print_job_queue_pop(print_job_queue_t *queue, print_job_t *out_job)
{
    if (print_job_queue_is_empty(queue)) {
        return false;
    }

    if (out_job != NULL) {
        *out_job = queue->jobs[queue->head];
    }
    queue->head = (queue->head + 1) % PRINT_JOB_QUEUE_CAPACITY;
    queue->count--;
    return true;
}
