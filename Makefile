CC := gcc
CFLAGS := -Wall -Wextra -Wpedantic -O2 \
	$(shell pkg-config --cflags gtk+-3.0 vte-2.91)
LDLIBS := $(shell pkg-config --libs gtk+-3.0 vte-2.91)

TARGET := et-mode-app
SRC := main.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
