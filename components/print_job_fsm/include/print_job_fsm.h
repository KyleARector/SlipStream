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

/* A job is either printable text, or a reference/handle to an image the
 * hardware-glue layer resolves to actual bitmap bytes when the job is
 * dequeued (M24) -- image payloads don't fit a queue slot sized for text,
 * so the queue only ever carries the reference, never image bytes
 * themselves. The queue treats both the same opaque way (push/pop don't
 * branch on type), which is what keeps this a single, type-agnostic
 * fixed-size FIFO instead of two parallel queues. */
typedef enum {
    PRINT_JOB_TYPE_TEXT = 0,
    PRINT_JOB_TYPE_IMAGE,
} print_job_type_t;

/* FIFO queue of pending print jobs, decoupled from the FSM itself so
 * incoming messages can queue up while one job is mid-flight. */
typedef struct {
    print_job_type_t type;
    /* Text content (PRINT_JOB_TYPE_TEXT), or an image reference/handle
     * string (PRINT_JOB_TYPE_IMAGE) -- same storage either way, since a
     * reference is just another short string. */
    char payload[PRINT_JOB_TEXT_MAX_LEN];
    size_t payload_len;
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

/* Copies payload (payload_len bytes, not required to be NUL-terminated)
 * into a new queue slot tagged with type. Fails if the queue is full or
 * payload_len is too long for PRINT_JOB_TEXT_MAX_LEN. */
bool print_job_queue_push(print_job_queue_t *queue, print_job_type_t type, const char *payload, size_t payload_len);

/* Pops the oldest job into *out_job (if non-NULL). Fails if empty. */
bool print_job_queue_pop(print_job_queue_t *queue, print_job_t *out_job);

#ifdef __cplusplus
}
#endif
