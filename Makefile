CC ?= cc
CFLAGS ?= -O2
WARNFLAGS := -std=c11 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
TARGET := copy
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)
DEPFILES := $(OBJECTS:.o=.d)

.PHONY: all clean install uninstall test check

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(WARNFLAGS) $(CFLAGS) $(OBJECTS) -o $@

src/%.o: src/%.c
	$(CC) $(WARNFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

test: $(TARGET)
	COPY_BIN="$(CURDIR)/$(TARGET)" python3 -m unittest discover -s tests -v

$(TARGET)-asan: $(SOURCES) $(wildcard src/*.h)
	$(CC) $(WARNFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=all \
		-g -O1 $(SOURCES) -o $@

check: $(TARGET)-asan
	COPY_BIN="$(CURDIR)/$(TARGET)-asan" \
		ASAN_OPTIONS="verify_asan_link_order=0:halt_on_error=1" \
		UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
		python3 -m unittest discover -s tests -v

clean:
	rm -f $(TARGET) $(TARGET)-asan $(OBJECTS) $(DEPFILES)

-include $(DEPFILES)
