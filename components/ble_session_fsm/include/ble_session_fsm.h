#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_SESSION_STATE_IDLE = 0,
    BLE_SESSION_STATE_ADVERTISING,
    BLE_SESSION_STATE_CONNECTED,
    BLE_SESSION_STATE_RECEIVING,
} ble_session_state_t;

typedef enum {
    BLE_SESSION_EVENT_BUTTON_PRESS,      /* IDLE -> ADVERTISING */
    BLE_SESSION_EVENT_PEER_CONNECTED,    /* ADVERTISING -> CONNECTED */
    BLE_SESSION_EVENT_RECEIVE_START,     /* CONNECTED -> RECEIVING */

    /* RECEIVING -> CONNECTED, not IDLE: finishing one message doesn't end
     * the session, since the BLE link is still up and more messages may
     * follow on it. Confirmed in M9 hardware review -- the original spec
     * diagram showed RECEIVING -> IDLE, but that made a still-connected
     * session look identical to a disconnected one (visible on real
     * hardware as the status LED going dark right after a message, even
     * though the phone was still connected). IDLE is reserved strictly for
     * "no BLE link at all". */
    BLE_SESSION_EVENT_RECEIVE_COMPLETE,

    /* ADVERTISING -> IDLE or CONNECTED -> IDLE. Covers both the named
     * "timeout path" from the spec (advertising/connection window elapsed)
     * and a peer disconnecting -- the spec's diagram doesn't distinguish
     * the two, and both land on the same destination state, so hardware
     * glue (M9) fires this event for either trigger. Confirmed in M9
     * hardware review: after a *disconnect* specifically (not an elapsed
     * advertising timeout with nobody ever connecting), the glue layer
     * re-fires BUTTON_PRESS immediately afterward to resume advertising
     * without requiring another physical button hold -- that's a policy
     * decision made in the hardware glue layer, not a new FSM transition. */
    BLE_SESSION_EVENT_TIMEOUT,
} ble_session_event_t;

typedef struct {
    ble_session_state_t state;
} ble_session_fsm_t;

void ble_session_fsm_init(ble_session_fsm_t *fsm);
ble_session_state_t ble_session_fsm_get_state(const ble_session_fsm_t *fsm);

/* Applies event to fsm's current state. Returns true and updates state if
 * the transition is legal; returns false and leaves state unchanged
 * otherwise (illegal transitions are rejected, never crash). */
bool ble_session_fsm_handle_event(ble_session_fsm_t *fsm, ble_session_event_t event);

#ifdef __cplusplus
}
#endif
