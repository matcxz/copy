CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
TARGET := copy
SOURCE := src/copy.c

.PHONY: all clean install uninstall test check

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(SOURCE) -o $@

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

test: $(TARGET)
	COPY_BIN="$(CURDIR)/$(TARGET)" python3 -m unittest discover -s tests -v

$(TARGET)-asan: $(SOURCE)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
		-g -O1 $(SOURCE) -o $@

check: $(TARGET)-asan
	COPY_BIN="$(CURDIR)/$(TARGET)-asan" \
		ASAN_OPTIONS="verify_asan_link_order=0:halt_on_error=1" \
		UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
		python3 -m unittest discover -s tests -v

clean:
	rm -f $(TARGET) $(TARGET)-asan
