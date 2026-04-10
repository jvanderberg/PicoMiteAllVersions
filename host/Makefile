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
         -I. -I..

LDFLAGS = -lm

# Source directory
VPATH = ..

# Core MMBasic sources (language runtime)
CORE_SRCS = MMBasic.c Commands.c Functions.c Operators.c MATHS.c Memory.c
CORE_SRCS += gfx_box_shared.c gfx_circle_shared.c gfx_line_shared.c gfx_pixel_shared.c gfx_cls_shared.c gfx_text_shared.c

# Bytecode VM sources
BC_SRCS = bc_compiler.c bc_compiler_core.c bc_compiler_expr.c \
          bc_compiler_stmt.c bc_vm.c bc_bridge.c bc_test.c bc_debug.c

# Host-specific sources
HOST_SRCS = host_stubs.c host_main.c

SRCS = $(HOST_SRCS) $(CORE_SRCS) $(BC_SRCS)
OBJS = $(SRCS:.c=.o)

TARGET = mmbasic_test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Host sources compile from current directory
host_stubs.o: host_stubs.c
	$(CC) $(CFLAGS) -c -o $@ $<

host_main.o: host_main.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Core sources from parent directory
%.o: ../%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
