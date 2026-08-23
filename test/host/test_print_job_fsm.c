#include "print_job_fsm.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- FSM: happy-path lifecycle ---- */

static void test_fsm_init_state_is_idle(void)
{
    print_job_fsm_t fsm;
    print_job_fsm_init(&fsm);
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_IDLE, print_job_fsm_get_state(&fsm));
}

static void test_fsm_full_legal_lifecycle(void)
{
    print_job_fsm_t fsm;
    print_job_fsm_init(&fsm);

    TEST_ASSERT_TRUE(print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_START));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_FORMATTING, print_job_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_FORMATTED));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_SENDING, print_job_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_SENT));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_PRINTING, print_job_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_PRINTED));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_COMPLETE, print_job_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_RESET));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_IDLE, print_job_fsm_get_state(&fsm));
}

static void run_error_from(print_job_state_t start_state, print_job_event_t event_to_reach_start)
{
    print_job_fsm_t fsm;
    print_job_fsm_init(&fsm);

    /* Drive to start_state via legal events. */
    print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_START); /* IDLE -> FORMATTING */
    if (start_state != PRINT_JOB_STATE_FORMATTING) {
        print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_FORMATTED); /* -> SENDING */
    }
    if (start_state == PRINT_JOB_STATE_PRINTING) {
        print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_SENT); /* -> PRINTING */
    }
    (void)event_to_reach_start;
    TEST_ASSERT_EQUAL_INT(start_state, print_job_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_ERROR));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_ERROR, print_job_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(print_job_fsm_handle_event(&fsm, PRINT_JOB_EVENT_RESET));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_IDLE, print_job_fsm_get_state(&fsm));
}

static void test_fsm_error_from_formatting(void)
{
    run_error_from(PRINT_JOB_STATE_FORMATTING, PRINT_JOB_EVENT_START);
}

static void test_fsm_error_from_sending(void)
{
    run_error_from(PRINT_JOB_STATE_SENDING, PRINT_JOB_EVENT_FORMATTED);
}

static void test_fsm_error_from_printing(void)
{
    run_error_from(PRINT_JOB_STATE_PRINTING, PRINT_JOB_EVENT_SENT);
}

/* ---- FSM: exhaustive legal/illegal transition matrix ----
 *
 * ERROR is only reachable from the three "active" states (FORMATTING,
 * SENDING, PRINTING) per the spec's state diagram -- not from IDLE or
 * COMPLETE. RESET (COMPLETE/ERROR -> IDLE) is an addition beyond the
 * spec's literal diagram, inferred so the FSM can be reused across queued
 * jobs; flagged to Kyle for review alongside this milestone.
 */

#define NUM_STATES 6
#define NUM_EVENTS 6

static const print_job_state_t k_all_states[NUM_STATES] = {
    PRINT_JOB_STATE_IDLE,       PRINT_JOB_STATE_FORMATTING, PRINT_JOB_STATE_SENDING,
    PRINT_JOB_STATE_PRINTING,   PRINT_JOB_STATE_COMPLETE,   PRINT_JOB_STATE_ERROR,
};

static const print_job_event_t k_all_events[NUM_EVENTS] = {
    PRINT_JOB_EVENT_START,     PRINT_JOB_EVENT_FORMATTED, PRINT_JOB_EVENT_SENT,
    PRINT_JOB_EVENT_PRINTED,   PRINT_JOB_EVENT_ERROR,     PRINT_JOB_EVENT_RESET,
};

/* Index by (state, event); PRINT_JOB_STATE_IDLE (0) means "not legal" here
 * since IDLE never appears as a legal *destination* of START. Use a sentinel
 * of -1 instead to keep that unambiguous. */
static int legal_next_state(print_job_state_t state, print_job_event_t event)
{
    if (state == PRINT_JOB_STATE_IDLE && event == PRINT_JOB_EVENT_START) {
        return PRINT_JOB_STATE_FORMATTING;
    }
    if (state == PRINT_JOB_STATE_FORMATTING && event == PRINT_JOB_EVENT_FORMATTED) {
        return PRINT_JOB_STATE_SENDING;
    }
    if (state == PRINT_JOB_STATE_FORMATTING && event == PRINT_JOB_EVENT_ERROR) {
        return PRINT_JOB_STATE_ERROR;
    }
    if (state == PRINT_JOB_STATE_SENDING && event == PRINT_JOB_EVENT_SENT) {
        return PRINT_JOB_STATE_PRINTING;
    }
    if (state == PRINT_JOB_STATE_SENDING && event == PRINT_JOB_EVENT_ERROR) {
        return PRINT_JOB_STATE_ERROR;
    }
    if (state == PRINT_JOB_STATE_PRINTING && event == PRINT_JOB_EVENT_PRINTED) {
        return PRINT_JOB_STATE_COMPLETE;
    }
    if (state == PRINT_JOB_STATE_PRINTING && event == PRINT_JOB_EVENT_ERROR) {
        return PRINT_JOB_STATE_ERROR;
    }
    if (state == PRINT_JOB_STATE_COMPLETE && event == PRINT_JOB_EVENT_RESET) {
        return PRINT_JOB_STATE_IDLE;
    }
    if (state == PRINT_JOB_STATE_ERROR && event == PRINT_JOB_EVENT_RESET) {
        return PRINT_JOB_STATE_IDLE;
    }
    return -1;
}

static void test_fsm_exhaustive_transition_matrix(void)
{
    char msg[64];

    for (int s = 0; s < NUM_STATES; s++) {
        for (int e = 0; e < NUM_EVENTS; e++) {
            print_job_state_t state = k_all_states[s];
            print_job_event_t event = k_all_events[e];
            int expected = legal_next_state(state, event);

            print_job_fsm_t fsm;
            print_job_fsm_init(&fsm);
            fsm.state = state;

            bool accepted = print_job_fsm_handle_event(&fsm, event);

            snprintf(msg, sizeof(msg), "state=%d event=%d", (int)state, (int)event);

            if (expected >= 0) {
                TEST_ASSERT_TRUE_MESSAGE(accepted, msg);
                TEST_ASSERT_EQUAL_INT_MESSAGE(expected, print_job_fsm_get_state(&fsm), msg);
            } else {
                TEST_ASSERT_FALSE_MESSAGE(accepted, msg);
                TEST_ASSERT_EQUAL_INT_MESSAGE(state, print_job_fsm_get_state(&fsm), msg);
            }
        }
    }
}

static void test_fsm_null_pointer_safety(void)
{
    /* Must not crash; illegal/no-op behavior for a missing fsm. */
    print_job_fsm_init(NULL);
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_STATE_IDLE, print_job_fsm_get_state(NULL));
    TEST_ASSERT_FALSE(print_job_fsm_handle_event(NULL, PRINT_JOB_EVENT_START));
}

/* ---- FIFO queue ---- */

static void test_queue_init_is_empty(void)
{
    print_job_queue_t q;
    print_job_queue_init(&q);
    TEST_ASSERT_TRUE(print_job_queue_is_empty(&q));
    TEST_ASSERT_FALSE(print_job_queue_is_full(&q));
    TEST_ASSERT_EQUAL_UINT(0, print_job_queue_count(&q));
}

static void test_queue_push_pop_fifo_order(void)
{
    print_job_queue_t q;
    print_job_queue_init(&q);

    TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, "first", 5, true));
    TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, "second", 6, true));
    TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, "third", 5, true));
    TEST_ASSERT_EQUAL_UINT(3, print_job_queue_count(&q));

    print_job_t job;

    TEST_ASSERT_TRUE(print_job_queue_pop(&q, &job));
    TEST_ASSERT_EQUAL_UINT(5, job.payload_len);
    TEST_ASSERT_EQUAL_STRING("first", job.payload);

    TEST_ASSERT_TRUE(print_job_queue_pop(&q, &job));
    TEST_ASSERT_EQUAL_UINT(6, job.payload_len);
    TEST_ASSERT_EQUAL_STRING("second", job.payload);

    TEST_ASSERT_TRUE(print_job_queue_pop(&q, &job));
    TEST_ASSERT_EQUAL_UINT(5, job.payload_len);
    TEST_ASSERT_EQUAL_STRING("third", job.payload);

    TEST_ASSERT_TRUE(print_job_queue_is_empty(&q));
}

static void test_queue_preserves_job_type(void)
{
    print_job_queue_t q;
    print_job_queue_init(&q);

    TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, "hello", 5, true));
    TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_IMAGE, "image-ref-42", 12, true));

    print_job_t job;

    TEST_ASSERT_TRUE(print_job_queue_pop(&q, &job));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_TYPE_TEXT, job.type);
    TEST_ASSERT_EQUAL_STRING("hello", job.payload);

    TEST_ASSERT_TRUE(print_job_queue_pop(&q, &job));
    TEST_ASSERT_EQUAL_INT(PRINT_JOB_TYPE_IMAGE, job.type);
    TEST_ASSERT_EQUAL_STRING("image-ref-42", job.payload);
}

static void test_queue_preserves_cut_after(void)
{
    print_job_queue_t q;
    print_job_queue_init(&q);

    TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_IMAGE, "image-ref", 9, false));
    TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, "From: @user", 11, true));

    print_job_t job;

    TEST_ASSERT_TRUE(print_job_queue_pop(&q, &job));
    TEST_ASSERT_FALSE(job.cut_after);

    TEST_ASSERT_TRUE(print_job_queue_pop(&q, &job));
    TEST_ASSERT_TRUE(job.cut_after);
}

static void test_queue_push_rejects_when_full(void)
{
    print_job_queue_t q;
    print_job_queue_init(&q);

    for (int i = 0; i < PRINT_JOB_QUEUE_CAPACITY; i++) {
        TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, "x", 1, true));
    }
    TEST_ASSERT_TRUE(print_job_queue_is_full(&q));
    TEST_ASSERT_FALSE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, "overflow", 8, true));
    TEST_ASSERT_EQUAL_UINT(PRINT_JOB_QUEUE_CAPACITY, print_job_queue_count(&q));
}

static void test_queue_pop_rejects_when_empty(void)
{
    print_job_queue_t q;
    print_job_queue_init(&q);

    print_job_t job;
    TEST_ASSERT_FALSE(print_job_queue_pop(&q, &job));
}

static void test_queue_push_rejects_text_too_long(void)
{
    print_job_queue_t q;
    print_job_queue_init(&q);

    char too_long[PRINT_JOB_TEXT_MAX_LEN + 1];
    memset(too_long, 'a', sizeof(too_long));

    TEST_ASSERT_FALSE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, too_long, sizeof(too_long), true));
    TEST_ASSERT_TRUE(print_job_queue_is_empty(&q));
}

static void test_queue_wraparound(void)
{
    print_job_queue_t q;
    print_job_queue_init(&q);

    /* Push/pop repeatedly past capacity so head/tail wrap the ring buffer
     * more than once; confirm order and content survive the wrap. */
    for (int round = 0; round < PRINT_JOB_QUEUE_CAPACITY * 3; round++) {
        char text[8];
        int len = snprintf(text, sizeof(text), "m%d", round);
        TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, text, (size_t)len, true));

        print_job_t job;
        TEST_ASSERT_TRUE(print_job_queue_pop(&q, &job));
        TEST_ASSERT_EQUAL_UINT((size_t)len, job.payload_len);
        TEST_ASSERT_EQUAL_STRING(text, job.payload);
    }
    TEST_ASSERT_TRUE(print_job_queue_is_empty(&q));
}

static void test_queue_null_pointer_safety(void)
{
    TEST_ASSERT_FALSE(print_job_queue_push(NULL, PRINT_JOB_TYPE_TEXT, "x", 1, true));
    TEST_ASSERT_FALSE(print_job_queue_pop(NULL, NULL));
    TEST_ASSERT_TRUE(print_job_queue_is_empty(NULL));
    TEST_ASSERT_FALSE(print_job_queue_is_full(NULL));
    TEST_ASSERT_EQUAL_UINT(0, print_job_queue_count(NULL));

    print_job_queue_t q;
    print_job_queue_init(&q);
    TEST_ASSERT_FALSE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, NULL, 1, true));

    /* pop with a NULL out_job still advances the queue, just discards the value. */
    TEST_ASSERT_TRUE(print_job_queue_push(&q, PRINT_JOB_TYPE_TEXT, "x", 1, true));
    TEST_ASSERT_TRUE(print_job_queue_pop(&q, NULL));
    TEST_ASSERT_TRUE(print_job_queue_is_empty(&q));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_fsm_init_state_is_idle);
    RUN_TEST(test_fsm_full_legal_lifecycle);
    RUN_TEST(test_fsm_error_from_formatting);
    RUN_TEST(test_fsm_error_from_sending);
    RUN_TEST(test_fsm_error_from_printing);
    RUN_TEST(test_fsm_exhaustive_transition_matrix);
    RUN_TEST(test_fsm_null_pointer_safety);

    RUN_TEST(test_queue_init_is_empty);
    RUN_TEST(test_queue_push_pop_fifo_order);
    RUN_TEST(test_queue_preserves_job_type);
    RUN_TEST(test_queue_preserves_cut_after);
    RUN_TEST(test_queue_push_rejects_when_full);
    RUN_TEST(test_queue_pop_rejects_when_empty);
    RUN_TEST(test_queue_push_rejects_text_too_long);
    RUN_TEST(test_queue_wraparound);
    RUN_TEST(test_queue_null_pointer_safety);

    return UNITY_END();
}
