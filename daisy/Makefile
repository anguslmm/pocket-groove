# Pocket Groove for the Electrosmith Daisy Seed (STM32H750, Cortex-M7, 480 MHz).
#   tools/daisy-setup.sh          once: fetch + build libDaisy at the pinned commit
#   make -C daisy                 stage 0 (STAGE=1: the breadboard prototype)
#   make -C daisy size            the memory map by component (tools/daisy_size.py)
#   tools/daisy-flash.sh          flash it (docs/DAISY.md "Flashing")
# Output: build/daisy/stage<N>/pocket-groove-daisy.{elf,bin,map}. The .bin is one
# file for one dfu-util write at 0x90040000: the 480 KB RAM image the Daisy
# bootloader copies to SRAM, then the ROM part (pg_daisy.lds .qspi_rom) at 0x900B8000.
#
# Our sources (lib/pg, daisy/core, daisy/app, daisy/src) build with the warnings of
# the other targets and must stay warning-free; libDaisy's headers are -isystem.
# libdaisy.a is built by its own Makefile (no -fshort-enums there, so none here
# either: the struct layouts in its headers must match the library's).

STAGE ?= 0
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
LIBDAISY ?= $(ROOT)/daisy/libDaisy
BUILD := $(ROOT)/build/daisy/stage$(STAGE)
TARGET := pocket-groove-daisy
LIBDAISY_COMMIT := $(shell sed -n 's/^LIBDAISY_COMMIT=//p' $(ROOT)/daisy/libdaisy.lock | cut -c1-7)
PG_VERSION ?= daisy-$(shell git -C $(ROOT) rev-parse --short HEAD 2>/dev/null || echo dev)

PREFIX := arm-none-eabi-
CC := $(PREFIX)gcc
CXX := $(PREFIX)g++
OBJCOPY := $(PREFIX)objcopy
SIZE := $(PREFIX)size

MCU := -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
OPT ?= -O2
DEFS := -DUSE_HAL_DRIVER -DSTM32H750xx -DSTM32H750IB -DHSE_VALUE=16000000 -DCORE_CM7 -DARM_MATH_CM7 \
        -DUSE_FULL_LL_DRIVER -DBOOT_APP -DFILEIO_ENABLE_FATFS_READER \
        -DPG_BULK_ATTR='__attribute__((section(".qspi_rom")))' \
        -DPG_DAISY_STAGE=$(STAGE) -DPG_VERSION='"$(PG_VERSION)"' -DPG_LIBDAISY_COMMIT='"$(LIBDAISY_COMMIT)"'
DAISY_INC := -isystem $(LIBDAISY)/src -isystem $(LIBDAISY)/src/sys -isystem $(LIBDAISY)/src/usbd \
             -isystem $(LIBDAISY)/src/usbh -isystem $(LIBDAISY) \
             -isystem $(LIBDAISY)/Drivers/CMSIS_5/CMSIS/Core/Include \
             -isystem $(LIBDAISY)/Drivers/CMSIS-Device/ST/STM32H7xx/Include \
             -isystem $(LIBDAISY)/Drivers/STM32H7xx_HAL_Driver/Inc \
             -isystem $(LIBDAISY)/Middlewares/ST/STM32_USB_Device_Library/Core/Inc \
             -isystem $(LIBDAISY)/Middlewares/Patched/ST/STM32_USB_Device_Library/Class/CDC/Inc \
             -isystem $(LIBDAISY)/Middlewares/ST/STM32_USB_Host_Library/Core/Inc \
             -isystem $(LIBDAISY)/Middlewares/ST/STM32_USB_Host_Library/Class/MSC/Inc \
             -isystem $(LIBDAISY)/Middlewares/ST/STM32_USB_Host_Library/Class/MIDI/Inc \
             -isystem $(LIBDAISY)/Drivers/STM32H7xx_HAL_Driver/Inc/Legacy \
             -isystem $(LIBDAISY)/Middlewares/Third_Party/FatFs/src
PG_INC := -I$(ROOT)/lib/pg/src -I$(ROOT)/daisy
WARN := -Wall -Wextra -Wdouble-promotion -Wfloat-conversion
COMMON := $(MCU) $(OPT) -g -ffunction-sections -fdata-sections -fno-common $(DEFS)
CXXFLAGS := $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-unwind-tables \
            -fno-use-cxa-atexit $(WARN)
# lib/pg and the portable Daisy code: no libDaisy headers at all.
PG_CXXFLAGS := $(CXXFLAGS) $(PG_INC)
DEV_CXXFLAGS := $(CXXFLAGS) $(PG_INC) $(DAISY_INC) -Wno-register
STARTUP_CFLAGS := $(COMMON) -std=gnu11 $(DAISY_INC) -include stm32h7xx.h

PG_SRCS := $(filter-out %_ref.cpp,$(shell find $(ROOT)/lib/pg/src/pg -name '*.cpp' | sort))
CORE_SRCS := $(sort $(wildcard $(ROOT)/daisy/core/*.cpp $(ROOT)/daisy/app/*.cpp))
DEV_SRCS := $(sort $(wildcard $(ROOT)/daisy/src/*.cpp))
STARTUP := $(LIBDAISY)/core/startup_stm32h750xx.c

obj = $(patsubst $(ROOT)/%,$(BUILD)/obj/%,$(1:.cpp=.o))
PG_OBJS := $(call obj,$(PG_SRCS))
CORE_OBJS := $(call obj,$(CORE_SRCS))
DEV_OBJS := $(call obj,$(DEV_SRCS))
STARTUP_OBJ := $(BUILD)/obj/startup_stm32h750xx.o
OBJS := $(STARTUP_OBJ) $(DEV_OBJS) $(CORE_OBJS) $(PG_OBJS)

LDSCRIPT := $(ROOT)/daisy/pg_daisy.lds
LDFLAGS := $(MCU) --specs=nano.specs --specs=nosys.specs -T$(LDSCRIPT) -Wl,--gc-sections \
           -Wl,-Map=$(BUILD)/$(TARGET).map,--cref -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments
LIBS := -L$(LIBDAISY)/build -ldaisy -lc -lm -lnosys

ELF := $(BUILD)/$(TARGET).elf
BIN := $(BUILD)/$(TARGET).bin

all: $(BIN)

$(LIBDAISY)/build/libdaisy.a:
	@echo "libDaisy is not built: run tools/daisy-setup.sh first" >&2; exit 1

# Code that only the main loop runs (the UI, the demos' builders, file formats, the
# macro text parser) is optimised for size: the 480 KB image is the scarce resource,
# and none of it is on the audio interrupt's path. Everything else -O2.
SIZE_OPT := $(addprefix $(ROOT)/lib/pg/src/pg/,ui/% demo/% io/% macro/macro_parse.cpp macro/library.cpp)
$(BUILD)/obj/lib/%.o: $(ROOT)/lib/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(PG_CXXFLAGS) $(if $(filter $(SIZE_OPT),$<),-Os) -MMD -MP $< -o $@

$(BUILD)/obj/daisy/core/%.o: $(ROOT)/daisy/core/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(PG_CXXFLAGS) -Wshadow -MMD -MP $< -o $@

$(BUILD)/obj/daisy/app/%.o: $(ROOT)/daisy/app/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(PG_CXXFLAGS) -Wshadow -MMD -MP $< -o $@

$(BUILD)/obj/daisy/src/%.o: $(ROOT)/daisy/src/%.cpp | $(LIBDAISY)/build/libdaisy.a
	@mkdir -p $(dir $@)
	$(CXX) -c $(DEV_CXXFLAGS) -MMD -MP $< -o $@

$(STARTUP_OBJ): $(STARTUP) | $(LIBDAISY)/build/libdaisy.a
	@mkdir -p $(dir $@)
	$(CC) -c $(STARTUP_CFLAGS) -w $< -o $@

$(ELF): $(OBJS) $(LDSCRIPT) $(LIBDAISY)/build/libdaisy.a
	$(CXX) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

# The image (everything the bootloader copies, padded to exactly 480 KB) + the ROM part.
$(BIN): $(ELF)
	$(OBJCOPY) -O binary -R .qspi_rom --pad-to=0x24078000 --gap-fill=0xFF $< $(BUILD)/image.bin
	$(OBJCOPY) -O binary -j .qspi_rom $< $(BUILD)/rom.bin
	cat $(BUILD)/image.bin $(BUILD)/rom.bin > $@
	@python3 $(ROOT)/tools/daisy_size.py --check $(ELF) $(BUILD)/image.bin $(BUILD)/rom.bin
	@$(SIZE) $<
	@echo "built $@ ($$(wc -c < $@ | tr -d ' ') bytes): flash it with tools/daisy-flash.sh"

size: $(ELF)
	@python3 $(ROOT)/tools/daisy_size.py $(ELF) $(BUILD)/$(TARGET).map

clean:
	rm -rf $(BUILD)

.PHONY: all size clean
-include $(OBJS:.o=.d)
