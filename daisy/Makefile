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
# stage 1's display: 0 the Adafruit 2090 (ILI9341 320x240), 1 an HX8357D 480x320, 2 the ST7789 240x135
PANEL ?= 0
# BUILD VARIANTS of the audio path, to compare on the board (tools/daisy-bench.sh;
# docs/DAISY.md section 11). Each defaults to today's build:
#   ITCM=1         the hot audio functions run from ITCM (tools/daisy_itcm.py picks them
#                  from daisy/itcm_profile.txt, the host's profile of the bench scenes, and
#                  a first link without ITCM gives their ARM sizes)
#   TABLES=dtcm    the bass's per-sample tables (sine and BLEP, 4.3 KB) in DTCM instead of
#                  AXI SRAM. (The lead's wavetables, 38.5 KB, do not fit: DTCM holds the
#                  engine's 77 KB state block and the stack.)
#   AUDIO_OPT=-O3  the audio path (lib/pg outside the -Os list) at -O3 instead of -O2; with
#                  AUDIO_OPT_FILES (patterns under lib/pg/src/pg, e.g. "lead/% drums/%") only
#                  those objects. -O3 on the whole audio path does not fit: +105 KB of code
#                  (measured: engine.cpp 43 -> 89 KB, bass.cpp 30 -> 56 KB)
#   ALIGN=1        the audio path's functions aligned to 32 bytes (a cache line), loops to 16
# A variant builds into its own directory: build/daisy/stage<N>[-itcm][-dtcm][-O3][-align].
ITCM ?= 0
TABLES ?= axi
AUDIO_OPT ?=
AUDIO_OPT_FILES ?=
ALIGN ?= 0
ifeq ($(filter $(TABLES),axi dtcm),)
$(error TABLES must be axi or dtcm)
endif
VARIANT_PARTS := $(if $(filter 1,$(ITCM)),itcm) $(if $(filter dtcm,$(TABLES)),dtcm) \
                 $(subst -,,$(AUDIO_OPT))$(if $(AUDIO_OPT),$(if $(AUDIO_OPT_FILES),part)) $(if $(filter 1,$(ALIGN)),align)
empty :=
space := $(empty) $(empty)
VARIANT := $(if $(strip $(VARIANT_PARTS)),$(subst $(space),-,$(strip $(VARIANT_PARTS))),base)
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
LIBDAISY ?= $(ROOT)/daisy/libDaisy
BUILD := $(ROOT)/build/daisy/stage$(STAGE)$(if $(filter-out base,$(VARIANT)),-$(VARIANT))
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
# PG_TAPE=0: the tape (lib/pg/src/pg/tape) is not on the Daisy yet; phase 2 gives it the
# SDRAM and the SD card (docs/STATE.md "TAPE PHASE 1").
DEFS := -DUSE_HAL_DRIVER -DSTM32H750xx -DSTM32H750IB -DHSE_VALUE=16000000 -DCORE_CM7 -DARM_MATH_CM7 \
        -DUSE_FULL_LL_DRIVER -DBOOT_APP -DFILEIO_ENABLE_FATFS_READER \
        -DPG_BULK_ATTR='__attribute__((section(".qspi_rom")))' \
        -DPG_DAISY_STAGE=$(STAGE) -DPG_DAISY_PANEL=$(PANEL) -DPG_VERSION='"$(PG_VERSION)"' -DPG_LIBDAISY_COMMIT='"$(LIBDAISY_COMMIT)"' \
        -DPG_TAPE=0 -DPG_VARIANT='"$(VARIANT)"' \
        -DPG_BUILD_FLAGS='"audio path $(OPT)$(if $(AUDIO_OPT), then $(AUDIO_OPT)$(if $(AUDIO_OPT_FILES), for $(AUDIO_OPT_FILES)))$(if $(filter 1,$(ALIGN)), aligned 32), main loop -Os; ITCM $(if $(filter 1,$(ITCM)),on,off); tables in $(if $(filter dtcm,$(TABLES)),DTCM,AXI SRAM)"'
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
# FatFS's code-page tables (libdaisy.a has ff.c but not this; libDaisy's core/Makefile adds it too).
CCSBCS := $(LIBDAISY)/Middlewares/Third_Party/FatFs/src/option/ccsbcs.c
CCSBCS_OBJ := $(BUILD)/obj/ccsbcs.o
OBJS := $(STARTUP_OBJ) $(CCSBCS_OBJ) $(DEV_OBJS) $(CORE_OBJS) $(PG_OBJS)

LDSCRIPT := $(ROOT)/daisy/pg_daisy.lds
LIBS := -L$(LIBDAISY)/build -ldaisy -lc -lm -lnosys

ELF := $(BUILD)/$(TARGET).elf
BIN := $(BUILD)/$(TARGET).bin

all: $(BIN)

$(LIBDAISY)/build/libdaisy.a:
	@echo "libDaisy is not built: run tools/daisy-setup.sh first" >&2; exit 1

# tools/daisy-setup.sh builds libDaisy -Os and leaves this marker (an older -O3 build
# leaves stage 1 without room): run the script again to rebuild it.
$(LIBDAISY)/build/pg-opt:
	@echo "libDaisy was built by an older tools/daisy-setup.sh (-O3): run tools/daisy-setup.sh again" >&2; exit 1

# Code that only the main loop runs (the UI, the demos' builders, file formats, the
# macro text parser; on the Daisy also the recorder, which runs from the UI frame) is
# optimised for size: the 480 KB image is the scarce resource, and none of it is on
# the audio interrupt's path. Everything else -O2.
SIZE_OPT := $(addprefix $(ROOT)/lib/pg/src/pg/,ui/% demo/% io/% macro/macro_parse.cpp macro/library.cpp \
              sampler/recorder.cpp)
# The Daisy's own code: -Os too, except what the audio interrupt runs (the pads'
# scan, the line input's fold-down, main.cpp's callback) and the display's strip
# expander (the main loop's heaviest loop).
DAISY_FAST := $(addprefix $(ROOT)/daisy/,core/pads.cpp core/line_mic.cpp core/panel.cpp src/main.cpp \
                src/usb_serial.cpp)
# The audio path: everything in lib/pg not in SIZE_OPT (the engine, the voices, the
# sampler, the instrument). AUDIO_OPT and ALIGN apply to it only.
AUDIO_OPT_SRCS := $(if $(AUDIO_OPT_FILES),$(addprefix $(ROOT)/lib/pg/src/pg/,$(addsuffix .cpp,$(patsubst %.cpp,%,$(AUDIO_OPT_FILES)))),%)
ALIGN_FLAGS := $(if $(filter 1,$(ALIGN)),-falign-functions=32 -falign-loops=16)
audio_flags = $(if $(filter $(AUDIO_OPT_SRCS),$(1)),$(AUDIO_OPT)) $(ALIGN_FLAGS)
$(BUILD)/obj/lib/%.o: $(ROOT)/lib/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(PG_CXXFLAGS) $(if $(filter $(SIZE_OPT),$<),-Os,$(call audio_flags,$<)) -MMD -MP $< -o $@

$(BUILD)/obj/daisy/core/%.o: $(ROOT)/daisy/core/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(PG_CXXFLAGS) $(if $(filter $(DAISY_FAST),$<),,-Os) -Wshadow -MMD -MP $< -o $@

# (bench.cpp plays host/tools/bench_instrument's busy pattern: host/common/grooves.h.)
$(BUILD)/obj/daisy/app/%.o: $(ROOT)/daisy/app/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(PG_CXXFLAGS) -I$(ROOT)/host -Os -Wshadow -MMD -MP $< -o $@

$(BUILD)/obj/daisy/src/%.o: $(ROOT)/daisy/src/%.cpp | $(LIBDAISY)/build/libdaisy.a
	@mkdir -p $(dir $@)
	$(CXX) -c $(DEV_CXXFLAGS) $(if $(filter $(DAISY_FAST),$<),,-Os) -MMD -MP $< -o $@

$(STARTUP_OBJ): $(STARTUP) | $(LIBDAISY)/build/libdaisy.a
	@mkdir -p $(dir $@)
	$(CC) -c $(STARTUP_CFLAGS) -w $< -o $@

$(CCSBCS_OBJ): $(CCSBCS) | $(LIBDAISY)/build/libdaisy.a
	@mkdir -p $(dir $@)
	$(CC) -c $(STARTUP_CFLAGS) -w $< -o $@

# The linker script's variant fragments (pg_daisy.lds BUILD VARIANTS), one directory per
# link: ld finds INCLUDEd files through -L.
LDDIR := $(BUILD)/ld
PRE_LDDIR := $(BUILD)/pre/ld
PRE_ELF := $(BUILD)/pre/$(TARGET).elf
DTCM_FRAG := $(if $(filter dtcm,$(TABLES)),*/pg/bass/bass_tables_data.o(.rodata .rodata.*))
VARIANT_STAMP := $(BUILD)/variant-$(VARIANT)
$(VARIANT_STAMP):
	@mkdir -p $(dir $@)
	@touch $@
%/pg_dtcm.ld: $(VARIANT_STAMP) $(ROOT)/daisy/Makefile
	@mkdir -p $(dir $@)
	@echo '$(DTCM_FRAG)' > $@
$(PRE_LDDIR)/pg_itcm.ld: $(VARIANT_STAMP) $(ROOT)/daisy/Makefile
	@mkdir -p $(dir $@)
	@echo > $@
LINK = $(CXX) $(OBJS) $(MCU) --specs=nano.specs --specs=nosys.specs -T$(LDSCRIPT) -Wl,--gc-sections \
       -Wl,-Map=$(3),--cref -Wl,--print-memory-usage -Wl,--no-warn-rwx-segments -L$(1) $(LIBS) -o $(2)
ifeq ($(ITCM),1)
# Two links: the first without ITCM gives the ARM sizes the choice needs.
$(PRE_ELF): $(OBJS) $(LDSCRIPT) $(LIBDAISY)/build/libdaisy.a $(LIBDAISY)/build/pg-opt \
            $(PRE_LDDIR)/pg_itcm.ld $(PRE_LDDIR)/pg_dtcm.ld
	$(call LINK,$(PRE_LDDIR),$@,$(BUILD)/pre/$(TARGET).map) > /dev/null
$(LDDIR)/pg_itcm.ld: $(PRE_ELF) $(ROOT)/daisy/itcm_profile.txt $(ROOT)/tools/daisy_itcm.py
	@mkdir -p $(dir $@)
	python3 $(ROOT)/tools/daisy_itcm.py $(ROOT)/daisy/itcm_profile.txt $(PRE_ELF) $@ --nm $(PREFIX)nm --objdump $(PREFIX)objdump
else
$(LDDIR)/pg_itcm.ld: $(VARIANT_STAMP)
	@mkdir -p $(dir $@)
	@echo > $@
endif

$(ELF): $(OBJS) $(LDSCRIPT) $(LIBDAISY)/build/libdaisy.a $(LIBDAISY)/build/pg-opt \
        $(LDDIR)/pg_itcm.ld $(LDDIR)/pg_dtcm.ld
	$(call LINK,$(LDDIR),$@,$(BUILD)/$(TARGET).map)

# The image (everything the bootloader copies, padded to exactly 480 KB) + the ROM part.
$(BIN): $(ELF)
	$(OBJCOPY) -O binary -R .qspi_rom --pad-to=0x24078000 --gap-fill=0xFF $< $(BUILD)/image.bin
	$(OBJCOPY) -O binary -j .qspi_rom $< $(BUILD)/rom.bin
	cat $(BUILD)/image.bin $(BUILD)/rom.bin > $@
	@python3 $(ROOT)/tools/daisy_size.py --check $(ELF) $(BUILD)/image.bin $(BUILD)/rom.bin
	@$(SIZE) $<
	@echo "built $@ ($$(wc -c < $@ | tr -d ' ') bytes, variant $(VARIANT)): flash it with tools/daisy-flash.sh$(if $(filter-out base,$(VARIANT)), $@)"

size: $(ELF)
	@python3 $(ROOT)/tools/daisy_size.py $(ELF) $(BUILD)/$(TARGET).map

clean:
	rm -rf $(BUILD)

.PHONY: all size clean
-include $(OBJS:.o=.d)
