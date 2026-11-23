CROSS_COMPILE ?= arm-linux-gnueabihf-
ARM_CC ?= $(CROSS_COMPILE)gcc
ARM_CC_FLAGS ?=
PYTHON ?= python3
HOST_CC ?= cc

BUILD := build/phase1
SOURCES := src/alice-kettle.c src/activity.c src/job.c src/listener.c \
	src/phrases.c src/quasar.c src/voice.c
HEADERS := $(wildcard src/*.h)
PROBE := scripts/bsa-management-probe.S
GATT_SOURCE := scripts/bsa-gatt-search.inc scripts/kettle-cache-attrs.inc
GATT_FLAGS := --defsym=GATT_REGISTER=1 --defsym=GATT_ENABLE=1 \
	--defsym=GATT_SEARCH=1 --defsym=GATT_REPLY_CACHE=1 \
	--defsym=GATT_CACHE_IMAGE=1 --defsym=GATT_CACHE_ONLY=1 \
	--defsym=GATT_CACHE_THEN_SEARCH=1 --defsym=GATT_TEMPERATURE=1

.PHONY: all clean check check-version check-python check-shell check-c test package
all: $(BUILD)/alice-kettle $(BUILD)/discovery \
	$(BUILD)/gatt-temperature $(BUILD)/gatt-stop $(BUILD)/gatt-boil

$(BUILD):
	mkdir -p $@

$(BUILD)/alice-kettle: $(SOURCES) $(HEADERS) | $(BUILD)
	$(ARM_CC) $(ARM_CC_FLAGS) -static -march=armv7-a -mfpu=vfpv3-d16 \
		-std=c11 -O2 -Wall -Wextra -Werror -Isrc -o $@ $(SOURCES)

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

# Host-side checks. These need no ARM toolchain and no BSA library.
check: check-version check-python check-shell check-c

check-version:
	scripts/check-version.sh

check-python:
	$(PYTHON) scripts/kettle-cache-load.py
	$(PYTHON) scripts/kettle-status-blobs.py
	$(PYTHON) -m py_compile scripts/*.py

check-shell:
	sh -n scripts/bsa-diagnostics.sh
	sh -n scripts/pack-yam.sh
	sh -n scripts/station-temperature-worker.sh
	sh -n packaging/yam/boot.sh
	sh -n packaging/yam/postinstall.sh
	sh -n packaging/yam/service.sh
	bash -n scripts/run-station.sh

check-c:
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -Isrc -fsyntax-only $(SOURCES)

test:
	HOST_CC="$(HOST_CC)" $(PYTHON) tests/test_voice_client.py

clean:
	rm -rf build

package:
	scripts/pack-yam.sh
