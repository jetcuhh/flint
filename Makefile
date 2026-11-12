.DEFAULT_GOAL := flint
CC ?= cc
CSTD = -std=c11
OPTFLAGS ?= -Os -ffunction-sections -fdata-sections
CFLAGS ?= -Wall -Wextra -Wno-unused-result
LDFLAGS ?=

TINY = -flto -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
TINY_LINK = -Wl,--gc-sections -Wl,--build-id=none -s

SRC = $(addprefix src/,flint.c ui.c ini.c theme.c cpu.c gpu.c mem.c disk.c net.c proc.c)
HDR = src/flint.h src/termbox2.h src/defcfg.h

src/defcfg.h: assets/flint.ini
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' assets/flint.ini > $@

flint: $(SRC) $(HDR)
	$(CC) $(CSTD) $(OPTFLAGS) $(TINY) $(CFLAGS) $(LDFLAGS) $(TINY_LINK) -o $@ $(SRC)

release: flint

ifeq ($(shell id -u),0)
PREFIX ?= /usr/local
else
PREFIX ?= $(HOME)/.local
endif

install: flint
	install -Dm755 flint $(DESTDIR)$(PREFIX)/bin/flint

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/flint

clean:
	rm -f flint src/defcfg.h
.PHONY: release install uninstall clean
