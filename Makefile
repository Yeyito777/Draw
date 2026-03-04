CC      = gcc
CFLAGS  = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -O2
LDFLAGS = -lX11 -lm
TARGET  = draw
SRC     = main.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)
