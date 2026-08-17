CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -Iunity
BUILD_DIR = build
COMPONENTS_DIR = ../../components

UNITY_SRC = unity/unity.c

.PHONY: all test clean

all: test

# --- pipeline sanity check ---
TRIVIAL_BIN = $(BUILD_DIR)/test_trivial
TRIVIAL_SRCS = $(UNITY_SRC) test_trivial.c

$(TRIVIAL_BIN): $(TRIVIAL_SRCS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TRIVIAL_SRCS) -o $@

# --- print_job_fsm ---
PRINT_JOB_FSM_DIR = $(COMPONENTS_DIR)/print_job_fsm
PRINT_JOB_FSM_BIN = $(BUILD_DIR)/test_print_job_fsm
PRINT_JOB_FSM_SRCS = $(UNITY_SRC) $(PRINT_JOB_FSM_DIR)/print_job_fsm.c test_print_job_fsm.c

$(PRINT_JOB_FSM_BIN): $(PRINT_JOB_FSM_SRCS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(PRINT_JOB_FSM_DIR)/include $(PRINT_JOB_FSM_SRCS) -o $@

# --- ble_session_fsm ---
BLE_SESSION_FSM_DIR = $(COMPONENTS_DIR)/ble_session_fsm
BLE_SESSION_FSM_BIN = $(BUILD_DIR)/test_ble_session_fsm
BLE_SESSION_FSM_SRCS = $(UNITY_SRC) $(BLE_SESSION_FSM_DIR)/ble_session_fsm.c test_ble_session_fsm.c

$(BLE_SESSION_FSM_BIN): $(BLE_SESSION_FSM_SRCS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(BLE_SESSION_FSM_DIR)/include $(BLE_SESSION_FSM_SRCS) -o $@

BINARIES = $(TRIVIAL_BIN) $(PRINT_JOB_FSM_BIN) $(BLE_SESSION_FSM_BIN)

test: $(BINARIES)
	@for bin in $(BINARIES); do \
		echo "== $$bin =="; \
		./$$bin || exit 1; \
	done

clean:
	rm -rf $(BUILD_DIR)
