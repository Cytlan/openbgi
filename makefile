# ------------------------------------------------------------------------------
#
# Makefile for OpenBGI
#
# ------------------------------------------------------------------------------

# Name of the executable
TARGET      := openbgi.dll

# Directories
SRCDIR      := src
BUILDDIR    := bin
TOOLSDIR    := tools

# Tools
TOOLS := $(TOOLSDIR)/inject

# C sources
CSOURCES    := $(shell find $(SRCDIR) -name '*.c')

# Additional libraries
LIBRARIES   := 

# ------------------------------------------------------------------------------
#
# Tools
#
# ------------------------------------------------------------------------------

# Compile & Link, Compile, Assemble and Link utilities
CC = i686-w64-mingw32-gcc-win32

# Compiler, assembler and linker options
CFLAGS = -O3
LDFLAGS = -O3 -shared -static-libgcc -lwinmm -lgdi32

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

all: setup $(TOOLS) $(BUILDDIR)/$(TARGET)

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