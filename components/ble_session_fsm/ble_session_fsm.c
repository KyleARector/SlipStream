#include "ble_session_fsm.h"

static ble_session_state_t next_state(ble_session_state_t state, ble_session_event_t event, bool *ok)
{
    switch (state) {
    case BLE_SESSION_STATE_IDLE:
        if (event == BLE_SESSION_EVENT_BUTTON_PRESS) {
            *ok = true;
            return BLE_SESSION_STATE_ADVERTISING;
        }
        break;

    case BLE_SESSION_STATE_ADVERTISING:
        if (event == BLE_SESSION_EVENT_PEER_CONNECTED) {
            *ok = true;
            return BLE_SESSION_STATE_CONNECTED;
        }
        if (event == BLE_SESSION_EVENT_TIMEOUT) {
            *ok = true;
            return BLE_SESSION_STATE_IDLE;
        }
        break;

    case BLE_SESSION_STATE_CONNECTED:
        if (event == BLE_SESSION_EVENT_RECEIVE_START) {
            *ok = true;
            return BLE_SESSION_STATE_RECEIVING;
        }
        if (event == BLE_SESSION_EVENT_TIMEOUT) {
            *ok = true;
            return BLE_SESSION_STATE_IDLE;
        }
        break;

    case BLE_SESSION_STATE_RECEIVING:
        if (event == BLE_SESSION_EVENT_RECEIVE_COMPLETE) {
            *ok = true;
            return BLE_SESSION_STATE_CONNECTED;
        }
        break;
    }

    *ok = false;
    return state;
}

void ble_session_fsm_init(ble_session_fsm_t *fsm)
{
    if (fsm == NULL) {
        return;
    }
    fsm->state = BLE_SESSION_STATE_IDLE;
}

ble_session_state_t ble_session_fsm_get_state(const ble_session_fsm_t *fsm)
{
    if (fsm == NULL) {
        return BLE_SESSION_STATE_IDLE;
    }
    return fsm->state;
}

bool ble_session_fsm_handle_event(ble_session_fsm_t *fsm, ble_session_event_t event)
{
    if (fsm == NULL) {
        return false;
    }

    bool ok = false;
    ble_session_state_t new_state = next_state(fsm->state, event, &ok);
    if (ok) {
        fsm->state = new_state;
    }
    return ok;
}
