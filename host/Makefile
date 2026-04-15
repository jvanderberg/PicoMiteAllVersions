# Host build for MMBasic interpreter + bytecode VM testing
#
# Builds a native macOS/Linux executable that can run BASIC programs
# through both the old interpreter and the new bytecode VM for comparison.

CC = gcc
CFLAGS = -std=gnu11 -g -O0 -Wall -Wno-unused-variable -Wno-unused-function \
         -Wno-unused-but-set-variable -Wno-misleading-indentation \
         -Wno-missing-braces -funsigned-char \
         -DPICOMITE -DMMBASIC_HOST \
         -include host_platform.h \
         -I. -I.. \
         -MMD -MP

LDFLAGS = -lm

# Source directory
VPATH = ..

# Core MMBasic sources (language runtime)
CORE_SRCS = MMBasic.c Commands.c Functions.c Operators.c MATHS.c Memory.c
CORE_SRCS += gfx_box_shared.c gfx_circle_shared.c gfx_line_shared.c gfx_pixel_shared.c gfx_cls_shared.c gfx_text_shared.c

# Bytecode VM sources
BC_SRCS = bc_alloc.c bc_bridge.c vm_host_fat.c ff.c ffunicode.c ffsystem.c \
          vm_sys_input.c vm_sys_graphics.c vm_sys_time.c vm_sys_audio.c vm_sys_pin.c vm_sys_file.c bc_source.c bc_compiler_core.c \
          bc_vm.c bc_runtime.c bc_debug.c

# Host-specific sources
HOST_SRCS = host_stubs_legacy.c host_main.c

SRCS = $(HOST_SRCS) $(CORE_SRCS) $(BC_SRCS)
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)

TARGET = mmbasic_test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Host sources compile from current directory
host_stubs_legacy.o: host_stubs_legacy.c
	$(CC) $(CFLAGS) -c -o $@ $<

host_main.o: host_main.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Core sources from parent directory
%.o: ../%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)

.PHONY: all clean
