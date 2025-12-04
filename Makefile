CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g
BUILD_DIR = build

all: $(BUILD_DIR)/parent $(BUILD_DIR)/child

$(BUILD_DIR)/parent: parent.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR)/child: child.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
	rm -f /tmp/os_lab3_mmap

.PHONY: all clean