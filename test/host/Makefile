CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -Iunity
BUILD_DIR = build
BIN = $(BUILD_DIR)/test_runner

# Pure-logic component sources land here as milestones add them
# (print_job_fsm, ble_session_fsm, escpos_formatter test files, etc.)
SRCS = unity/unity.c test_trivial.c

.PHONY: all test clean

all: test

$(BIN): $(SRCS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SRCS) -o $(BIN)

test: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BUILD_DIR)
