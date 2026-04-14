CC      = gcc
CFLAGS  = -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -O2
LDFLAGS = -lX11 -lXext -lm
TARGET  = draw
SRC     = main.c

PREFIX = $(HOME)/.local/bin

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: $(TARGET)
	mkdir -p $(PREFIX)
	cp -f $(TARGET) $(PREFIX)/$(TARGET)

clean:
	rm -f $(TARGET)
