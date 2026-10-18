CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
TARGET := copy
SOURCE := src/copy.c

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $@

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
