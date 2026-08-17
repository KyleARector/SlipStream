#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRINT_JOB_QUEUE_CAPACITY 8
#define PRINT_JOB_TEXT_MAX_LEN   256

typedef enum {
    PRINT_JOB_STATE_IDLE = 0,
    PRINT_JOB_STATE_FORMATTING,
    PRINT_JOB_STATE_SENDING,
    PRINT_JOB_STATE_PRINTING,
    PRINT_JOB_STATE_COMPLETE,
    PRINT_JOB_STATE_ERROR,
} print_job_state_t;

typedef enum {
    PRINT_JOB_EVENT_START,      /* IDLE -> FORMATTING (a job was dequeued) */
    PRINT_JOB_EVENT_FORMATTED,  /* FORMATTING -> SENDING */
    PRINT_JOB_EVENT_SENT,       /* SENDING -> PRINTING */
    PRINT_JOB_EVENT_PRINTED,    /* PRINTING -> COMPLETE */
    PRINT_JOB_EVENT_ERROR,      /* FORMATTING/SENDING/PRINTING -> ERROR */
    PRINT_JOB_EVENT_RESET,      /* COMPLETE/ERROR -> IDLE */
} print_job_event_t;

typedef struct {
    print_job_state_t state;
} print_job_fsm_t;

void print_job_fsm_init(print_job_fsm_t *fsm);
print_job_state_t print_job_fsm_get_state(const print_job_fsm_t *fsm);

/* Applies event to fsm's current state. Returns true and updates state if
 * the transition is legal; returns false and leaves state unchanged
 * otherwise (illegal transitions are rejected, never crash). */
bool print_job_fsm_handle_event(print_job_fsm_t *fsm, print_job_event_t event);

/* FIFO queue of pending print jobs, decoupled from the FSM itself so
 * incoming messages can queue up while one job is mid-flight. */
typedef struct {
    char text[PRINT_JOB_TEXT_MAX_LEN];
    size_t text_len;
} print_job_t;

typedef struct {
    print_job_t jobs[PRINT_JOB_QUEUE_CAPACITY];
    size_t head;
    size_t count;
} print_job_queue_t;

void print_job_queue_init(print_job_queue_t *queue);
bool print_job_queue_is_empty(const print_job_queue_t *queue);
bool print_job_queue_is_full(const print_job_queue_t *queue);
size_t print_job_queue_count(const print_job_queue_t *queue);

/* Copies text (text_len bytes, not required to be NUL-terminated) into a
 * new queue slot. Fails if the queue is full or text_len is too long for
 * PRINT_JOB_TEXT_MAX_LEN. */
bool print_job_queue_push(print_job_queue_t *queue, const char *text, size_t text_len);

/* Pops the oldest job into *out_job (if non-NULL). Fails if empty. */
bool print_job_queue_pop(print_job_queue_t *queue, print_job_t *out_job);

#ifdef __cplusplus
}
#endif
