CC ?= clang
CFLAGS ?= -O3 -Wall -Wextra -Wno-deprecated-declarations
FRAMEWORKS = -framework CoreMIDI -framework CoreFoundation
PREFIX ?= $(HOME)/.local
BINDIR = $(PREFIX)/bin
TARGET = midi-router

all: $(TARGET)

$(TARGET): midi-router.c
	$(CC) $(CFLAGS) $(FRAMEWORKS) -o $(TARGET) midi-router.c

install: $(TARGET)
	mkdir -p $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	@echo "Installed $(TARGET) to $(BINDIR)/$(TARGET)"

clean:
	rm -f $(TARGET)

.PHONY: all install clean
