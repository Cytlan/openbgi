
TARGET := inject.exe
CC     := i686-w64-mingw32-gcc-win32

CFLAGS := 

# C sources
CSOURCES    := inject.c

# Get object names
OBJS = $(CSOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
