#include "ble_session_fsm.h"
#include "unity.h"

#include <stdio.h>

void setUp(void) {}
void tearDown(void) {}

/* ---- happy-path lifecycle ---- */

static void test_fsm_init_state_is_idle(void)
{
    ble_session_fsm_t fsm;
    ble_session_fsm_init(&fsm);
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_IDLE, ble_session_fsm_get_state(&fsm));
}

static void test_fsm_full_legal_lifecycle(void)
{
    ble_session_fsm_t fsm;
    ble_session_fsm_init(&fsm);

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_BUTTON_PRESS));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_ADVERTISING, ble_session_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_PEER_CONNECTED));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_CONNECTED, ble_session_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_RECEIVE_START));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_RECEIVING, ble_session_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_RECEIVE_COMPLETE));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_CONNECTED, ble_session_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_TIMEOUT));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_IDLE, ble_session_fsm_get_state(&fsm));
}

/* A single BLE connection must be able to receive multiple messages in a
 * row without dropping back to IDLE between them -- that's the whole
 * reason RECEIVING lands back on CONNECTED rather than IDLE. */
static void test_fsm_multiple_messages_on_one_connection(void)
{
    ble_session_fsm_t fsm;
    ble_session_fsm_init(&fsm);

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_BUTTON_PRESS));
    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_PEER_CONNECTED));

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_RECEIVE_START));
        TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_RECEIVING, ble_session_fsm_get_state(&fsm));

        TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_RECEIVE_COMPLETE));
        TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_CONNECTED, ble_session_fsm_get_state(&fsm));
    }
}

/* ---- timeout paths ---- */

static void test_fsm_timeout_from_advertising(void)
{
    ble_session_fsm_t fsm;
    ble_session_fsm_init(&fsm);

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_BUTTON_PRESS));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_ADVERTISING, ble_session_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_TIMEOUT));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_IDLE, ble_session_fsm_get_state(&fsm));
}

static void test_fsm_timeout_from_connected(void)
{
    ble_session_fsm_t fsm;
    ble_session_fsm_init(&fsm);

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_BUTTON_PRESS));
    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_PEER_CONNECTED));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_CONNECTED, ble_session_fsm_get_state(&fsm));

    TEST_ASSERT_TRUE(ble_session_fsm_handle_event(&fsm, BLE_SESSION_EVENT_TIMEOUT));
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_IDLE, ble_session_fsm_get_state(&fsm));
}

/* ---- exhaustive legal/illegal transition matrix ----
 *
 * Legal transitions per the spec's diagram:
 *   IDLE        --BUTTON_PRESS-->      ADVERTISING
 *   ADVERTISING --PEER_CONNECTED-->    CONNECTED
 *   ADVERTISING --TIMEOUT-->           IDLE
 *   CONNECTED   --RECEIVE_START-->     RECEIVING
 *   CONNECTED   --TIMEOUT-->           IDLE
 *   RECEIVING   --RECEIVE_COMPLETE-->  CONNECTED
 * Every other (state, event) pair must be rejected as a no-op.
 */

#define NUM_STATES 4
#define NUM_EVENTS 5

static const ble_session_state_t k_all_states[NUM_STATES] = {
    BLE_SESSION_STATE_IDLE,
    BLE_SESSION_STATE_ADVERTISING,
    BLE_SESSION_STATE_CONNECTED,
    BLE_SESSION_STATE_RECEIVING,
};

static const ble_session_event_t k_all_events[NUM_EVENTS] = {
    BLE_SESSION_EVENT_BUTTON_PRESS,
    BLE_SESSION_EVENT_PEER_CONNECTED,
    BLE_SESSION_EVENT_RECEIVE_START,
    BLE_SESSION_EVENT_RECEIVE_COMPLETE,
    BLE_SESSION_EVENT_TIMEOUT,
};

static int legal_next_state(ble_session_state_t state, ble_session_event_t event)
{
    if (state == BLE_SESSION_STATE_IDLE && event == BLE_SESSION_EVENT_BUTTON_PRESS) {
        return BLE_SESSION_STATE_ADVERTISING;
    }
    if (state == BLE_SESSION_STATE_ADVERTISING && event == BLE_SESSION_EVENT_PEER_CONNECTED) {
        return BLE_SESSION_STATE_CONNECTED;
    }
    if (state == BLE_SESSION_STATE_ADVERTISING && event == BLE_SESSION_EVENT_TIMEOUT) {
        return BLE_SESSION_STATE_IDLE;
    }
    if (state == BLE_SESSION_STATE_CONNECTED && event == BLE_SESSION_EVENT_RECEIVE_START) {
        return BLE_SESSION_STATE_RECEIVING;
    }
    if (state == BLE_SESSION_STATE_CONNECTED && event == BLE_SESSION_EVENT_TIMEOUT) {
        return BLE_SESSION_STATE_IDLE;
    }
    if (state == BLE_SESSION_STATE_RECEIVING && event == BLE_SESSION_EVENT_RECEIVE_COMPLETE) {
        return BLE_SESSION_STATE_CONNECTED;
    }
    return -1;
}

static void test_fsm_exhaustive_transition_matrix(void)
{
    char msg[64];

    for (int s = 0; s < NUM_STATES; s++) {
        for (int e = 0; e < NUM_EVENTS; e++) {
            ble_session_state_t state = k_all_states[s];
            ble_session_event_t event = k_all_events[e];
            int expected = legal_next_state(state, event);

            ble_session_fsm_t fsm;
            ble_session_fsm_init(&fsm);
            fsm.state = state;

            bool accepted = ble_session_fsm_handle_event(&fsm, event);

            snprintf(msg, sizeof(msg), "state=%d event=%d", (int)state, (int)event);

            if (expected >= 0) {
                TEST_ASSERT_TRUE_MESSAGE(accepted, msg);
                TEST_ASSERT_EQUAL_INT_MESSAGE(expected, ble_session_fsm_get_state(&fsm), msg);
            } else {
                TEST_ASSERT_FALSE_MESSAGE(accepted, msg);
                TEST_ASSERT_EQUAL_INT_MESSAGE(state, ble_session_fsm_get_state(&fsm), msg);
            }
        }
    }
}

static void test_fsm_null_pointer_safety(void)
{
    /* Must not crash; illegal/no-op behavior for a missing fsm. */
    ble_session_fsm_init(NULL);
    TEST_ASSERT_EQUAL_INT(BLE_SESSION_STATE_IDLE, ble_session_fsm_get_state(NULL));
    TEST_ASSERT_FALSE(ble_session_fsm_handle_event(NULL, BLE_SESSION_EVENT_BUTTON_PRESS));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_fsm_init_state_is_idle);
    RUN_TEST(test_fsm_full_legal_lifecycle);
    RUN_TEST(test_fsm_multiple_messages_on_one_connection);
    RUN_TEST(test_fsm_timeout_from_advertising);
    RUN_TEST(test_fsm_timeout_from_connected);
    RUN_TEST(test_fsm_exhaustive_transition_matrix);
    RUN_TEST(test_fsm_null_pointer_safety);

    return UNITY_END();
}
