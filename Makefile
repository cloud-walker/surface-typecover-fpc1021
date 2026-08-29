CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11
CPPFLAGS += -Isrc
LDLIBS = -lusb-1.0

BUILD = build
COMMON = $(BUILD)/fpc_trace.o $(BUILD)/fpc_device.o

all: $(BUILD)/fpc_capture $(BUILD)/fpc_probe

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/%.o: tools/%.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/fpc_capture: $(BUILD)/fpc_capture.o $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/fpc_probe: $(BUILD)/fpc_probe.o $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
