CC ?= gcc
CXX ?= g++
NODE ?= node
PYTHON ?= python3

MIXXX_BUILD ?= /home/pi/mixxx-d2-build
CTLRA_ROOT ?= /home/pi/openAV-Ctlra
OUTPUT_DIR ?= $(CTLRA_ROOT)/build
PROTO_DIR := $(MIXXX_BUILD)/src/proto
CTLRA_LIB_DIR := $(CTLRA_ROOT)/build/ctlra

CFLAGS ?= -O2
CXXFLAGS ?= -O2
WARNINGS := -Wall -Wextra -Werror
FREETYPE_CFLAGS := $(shell pkg-config --cflags freetype2)
FREETYPE_LIBS := $(shell pkg-config --libs freetype2)

.PHONY: all bridge extractors test test-native test-js clean

all: bridge extractors

bridge: $(OUTPUT_DIR)/d2_bridge_metadata

extractors: $(OUTPUT_DIR)/d2_beatgrid_extract \
	$(OUTPUT_DIR)/d2_waveform_extract

$(OUTPUT_DIR):
	mkdir -p $@

$(OUTPUT_DIR)/d2_bridge_metadata: d2_bridge.c | $(OUTPUT_DIR)
	$(CC) -std=gnu11 $(CFLAGS) $(WARNINGS) \
		-I$(CTLRA_ROOT) -I$(CTLRA_ROOT)/ctlra $(FREETYPE_CFLAGS) \
		$< -L$(CTLRA_LIB_DIR) -Wl,-rpath,'$$ORIGIN/ctlra' \
		-lctlra -lasound -lsqlite3 $(FREETYPE_LIBS) -lm -lpthread -o $@

$(OUTPUT_DIR)/d2_beatgrid_extract: d2_beatgrid_extract.cpp \
	$(PROTO_DIR)/beats.pb.cc | $(OUTPUT_DIR)
	$(CXX) -std=c++17 $(CXXFLAGS) $(WARNINGS) -I$(PROTO_DIR) \
		$^ -lsqlite3 -lprotobuf-lite -pthread -o $@

$(OUTPUT_DIR)/d2_waveform_extract: d2_waveform_extract.cpp \
	$(PROTO_DIR)/waveform.pb.cc | $(OUTPUT_DIR)
	$(CXX) -std=c++17 $(CXXFLAGS) $(WARNINGS) -I$(PROTO_DIR) \
		$^ -lz -lprotobuf-lite -pthread -o $@

test: test-native test-js

test-native: all
	$(OUTPUT_DIR)/d2_beatgrid_extract --self-test
	LD_LIBRARY_PATH=$(CTLRA_LIB_DIR) \
		$(OUTPUT_DIR)/d2_bridge_metadata --beat-geometry-test
	LD_LIBRARY_PATH=$(CTLRA_LIB_DIR) \
		$(OUTPUT_DIR)/d2_bridge_metadata --load-reject-notice-test
	LD_LIBRARY_PATH=$(CTLRA_LIB_DIR) \
		$(OUTPUT_DIR)/d2_bridge_metadata --functionality-contract-test

test-js:
	@command -v $(NODE) >/dev/null 2>&1 || { \
		echo "Node.js is required for controller regression tests" >&2; exit 1; }
	@command -v $(PYTHON) >/dev/null 2>&1 || { \
		echo "Python 3 is required for skin/source regression tests" >&2; exit 1; }
	$(NODE) ../tests/test_d2_mapping.js
	$(NODE) ../tests/test_d2_complete_controls.js
	$(NODE) ../tests/test_d2_comprehensive.js
	$(NODE) ../tests/test_d2_load_polling.js
	$(NODE) ../tests/test_d2_fx_roundtrip.js
	$(PYTHON) ../tests/test_zed_fx_layout.py
	$(PYTHON) ../tests/test_time_display_modes.py
	$(PYTHON) ../tests/test_zed_launch_animation.py

clean:
	rm -f $(OUTPUT_DIR)/d2_bridge_metadata \
		$(OUTPUT_DIR)/d2_beatgrid_extract \
		$(OUTPUT_DIR)/d2_waveform_extract
