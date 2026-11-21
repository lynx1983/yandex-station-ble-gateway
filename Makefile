CROSS_COMPILE ?= arm-linux-gnueabihf-
ARM_CC ?= $(CROSS_COMPILE)gcc
ARM_CC_FLAGS ?=
PYTHON ?= python3

BUILD := build/phase1
PROBE := scripts/bsa-management-probe.S
GATT_SOURCE := scripts/bsa-gatt-search.inc scripts/kettle-cache-attrs.inc
GATT_FLAGS := --defsym=GATT_REGISTER=1 --defsym=GATT_ENABLE=1 \
	--defsym=GATT_SEARCH=1 --defsym=GATT_REPLY_CACHE=1 \
	--defsym=GATT_CACHE_IMAGE=1 --defsym=GATT_CACHE_ONLY=1 \
	--defsym=GATT_CACHE_THEN_SEARCH=1 --defsym=GATT_TEMPERATURE=1

.PHONY: all
all: $(BUILD)/alice-kettle $(BUILD)/discovery \
	$(BUILD)/gatt-temperature $(BUILD)/gatt-stop $(BUILD)/gatt-boil

$(BUILD):
	mkdir -p $@

$(BUILD)/alice-kettle: src/alice-kettle.c | $(BUILD)
	$(ARM_CC) $(ARM_CC_FLAGS) -static -march=armv7-a -mfpu=vfpv3-d16 \
		-std=c11 -O2 -Wall -Wextra -Werror -o $@ $<

$(BUILD)/kettle-status-blobs.inc: scripts/kettle-status-blobs.py | $(BUILD)
	$(PYTHON) $<

$(BUILD)/discovery.o: $(PROBE) $(GATT_SOURCE) | $(BUILD)
	$(CROSS_COMPILE)as --defsym=DISCOVERY=1 -o $@ $<

$(BUILD)/gatt-stop.o: OP_FLAGS := --defsym=GATT_STOP=1
$(BUILD)/gatt-boil.o: OP_FLAGS := --defsym=GATT_BOIL=1
$(BUILD)/gatt-%.o: $(PROBE) $(GATT_SOURCE) $(BUILD)/kettle-status-blobs.inc
	$(CROSS_COMPILE)as $(GATT_FLAGS) $(OP_FLAGS) -o $@ $<

$(BUILD)/discovery $(BUILD)/gatt-temperature $(BUILD)/gatt-stop $(BUILD)/gatt-boil: %: %.o $(BUILD)/libbsa.so
	$(CROSS_COMPILE)ld --dynamic-linker /lib/ld-linux-armhf.so.3 \
		--allow-shlib-undefined -L$(BUILD) -o $@ $< -lbsa
