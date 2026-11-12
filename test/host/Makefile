# Host tests of ESP32MQTTClient: make -C test/host
#
# The library is compiled against the real mqtt_client.h of the esp-mqtt revisions pinned by
# ESP-IDF v5.3.3 and v4.4.6 (downloaded once into build/), with stubs for the other ESP-IDF
# headers, and run under the address/undefined-behaviour and thread sanitizers.

CXX ?= c++

# esp-mqtt submodule revisions of ESP-IDF v5.3.3 and v4.4.6
ESP_MQTT_IDF5 := cac1552e62b0474c162547b7cce345d7cd1aecfe
ESP_MQTT_IDF4 := bb9c8af9d552b608dd3aabf9617bde757a538ebe
ESP_MQTT_URL  := https://raw.githubusercontent.com/espressif/esp-mqtt

BUILD := build
LIB_SRC := ../../src/ESP32MQTTClient.cpp
LIB_HDR := ../../src/ESP32MQTTClient.h
STUBS := $(wildcard stubs/*.h stubs/*/*.h)

ASAN := address,undefined -fno-sanitize=vptr -fno-sanitize-recover=undefined
TSAN := thread
# The library is built like on the device (no RTTI, no exceptions), the test program normally
LIB_FLAGS := -fno-rtti -fno-exceptions
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers

# name, IDF headers, C++ standard of the library, sanitizer, extra flags
#   IDF 5.3 builds C++ as gnu++2b; gnu++11 is used for IDF 4.4 to check the oldest standard
#   lowhz: CONFIG_FREERTOS_HZ = 50, where pdMS_TO_TICKS(10) is 0
VARIANTS := idf5-asan idf4-asan idf5-tsan idf4-tsan idf5-lowhz-asan

define variant
$(BUILD)/$(1): test_main.cpp $(LIB_SRC) $(LIB_HDR) $(STUBS) $(BUILD)/$(2)/mqtt_client.h
	@mkdir -p $(BUILD)
	$(CXX) -std=$(3) -g -O1 $(LIB_FLAGS) -fsanitize=$(4) $(WARN) $(5) -I$(BUILD)/$(2) -Istubs/$(2) -Istubs -I../../src -c $(LIB_SRC) -o $$@.lib.o
	$(CXX) -std=gnu++17 -g -O1 -fsanitize=$(4) $(WARN) $(5) -I$(BUILD)/$(2) -Istubs/$(2) -Istubs -I../../src test_main.cpp $$@.lib.o -o $$@ -pthread
endef

.PHONY: all test clean
all: test

$(eval $(call variant,idf5-asan,idf5,gnu++2b,$(ASAN),))
$(eval $(call variant,idf4-asan,idf4,gnu++11,$(ASAN),))
$(eval $(call variant,idf5-tsan,idf5,gnu++2b,$(TSAN),))
$(eval $(call variant,idf4-tsan,idf4,gnu++11,$(TSAN),))
$(eval $(call variant,idf5-lowhz-asan,idf5,gnu++2b,$(ASAN),-DTEST_TICK_PERIOD_MS=20))

$(BUILD)/idf5/mqtt_client.h:
	@mkdir -p $(dir $@)
	curl -fsSL -o $@ $(ESP_MQTT_URL)/$(ESP_MQTT_IDF5)/include/mqtt_client.h

$(BUILD)/idf4/mqtt_client.h:
	@mkdir -p $(dir $@)
	curl -fsSL -o $@ $(ESP_MQTT_URL)/$(ESP_MQTT_IDF4)/include/mqtt_client.h

test: $(addprefix $(BUILD)/,$(VARIANTS))
	@set -e; for v in $^; do echo "== $$v"; ./$$v; done

clean:
	rm -rf $(BUILD)
