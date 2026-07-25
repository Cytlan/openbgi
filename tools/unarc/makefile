# ------------------------------------------------------------------------------
#
# Makefile for OpenBGI
#
# ------------------------------------------------------------------------------

# Name of the executable
TARGET      := unarc

# Directories
SRCDIR      := src
BUILDDIR    := bin

# C sources
CSOURCES    := $(shell find $(SRCDIR) -name '*.c')

# Additional libraries
LIBRARIES   := 

INCDIR := src

# ------------------------------------------------------------------------------
#
# Tools
#
# ------------------------------------------------------------------------------

# Compile & Link, Compile, Assemble and Link utilities
CC = gcc

# Compiler, assembler and linker options
CFLAGS = -O3 -DSPNG_USE_MINIZ
LDFLAGS = -O3 -lm

# System utilities
RM = rm -f

# ------------------------------------------------------------------------------
#
# Flags and argument finalization
#
# ------------------------------------------------------------------------------

# Finalizing flags
CFLAGS += $(patsubst %,-I%,$(INCDIR))

# Get object names
OBJS_ = $(CSOURCES:.c=.o)
OBJS = $(patsubst $(SRCDIR)/%,$(BUILDDIR)/%,$(OBJS_)) $(LIBRARIES)
PATHS = $(sort $(dir $(OBJS)))

# ------------------------------------------------------------------------------
#
# Targets
#
# ------------------------------------------------------------------------------

all: setup $(BUILDDIR)/$(TARGET)

setup:
	mkdir -p $(PATHS)

$(BUILDDIR)/$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $(BUILDDIR)/$(TARGET)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(TOOLS):
	echo $@
	$(MAKE) -C $@

clean:
	$(RM) $(OBJS) $(BUILDDIR)/$(TARGET)

.PHONY: all setup $(TOOLS)

# EOF --------------------------------------------------------------------------