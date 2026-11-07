CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr
bindir = $(PREFIX)/bin
datadir = $(PREFIX)/share
mandir = $(datadir)/man
docdir = $(datadir)/doc/ctermfluid
INSTALL ?= install

TARGET = ctermfluid
VERSION = $(shell cat VERSION)
NCURSES_CFLAGS = $(shell $(PKG_CONFIG) --cflags ncursesw 2>/dev/null)
NCURSES_LIBS = $(shell $(PKG_CONFIG) --libs ncursesw 2>/dev/null || printf '%s' '-lncursesw')
LDLIBS ?= $(NCURSES_LIBS) -lm
PROGRAMS = programs/default.conf programs/crossflow.conf
DISTDIR = ctermfluid-$(VERSION)
DISTFILES = ctermfluid.c config.c config.h embed-programs.awk Makefile VERSION README.md LICENSE .gitignore ctermfluid.1 doc/demo.svg $(PROGRAMS) .github/workflows/ci.yml

.PHONY: all clean install uninstall dist FORCE
all: $(TARGET)

build_config.h: FORCE
	@printf '#define CTERMFLUID_VERSION "%s"\n' '$(VERSION)' > build_config.h.tmp
	@cmp -s build_config.h.tmp $@ || cp build_config.h.tmp $@
	@rm -f build_config.h.tmp

program_defaults.h: $(PROGRAMS) embed-programs.awk
	awk -f embed-programs.awk $(PROGRAMS) > $@.tmp
	mv $@.tmp $@

$(TARGET): ctermfluid.c config.c config.h build_config.h program_defaults.h
	$(CC) $(CPPFLAGS) $(NCURSES_CFLAGS) $(CFLAGS) $(LDFLAGS) ctermfluid.c config.c $(LDLIBS) -o $@

install: $(TARGET)
	$(INSTALL) -d "$(DESTDIR)$(bindir)" "$(DESTDIR)$(mandir)/man1" "$(DESTDIR)$(docdir)"
	$(INSTALL) -m 755 $(TARGET) "$(DESTDIR)$(bindir)/$(TARGET)"
	$(INSTALL) -m 644 ctermfluid.1 "$(DESTDIR)$(mandir)/man1/ctermfluid.1"
	$(INSTALL) -m 644 LICENSE "$(DESTDIR)$(docdir)/LICENSE"

uninstall:
	rm -f "$(DESTDIR)$(bindir)/$(TARGET)" "$(DESTDIR)$(mandir)/man1/ctermfluid.1"
	rm -f "$(DESTDIR)$(docdir)/LICENSE"
	@rmdir "$(DESTDIR)$(docdir)" 2>/dev/null || true

dist:
	test -f LICENSE
	test ! -e "$(DISTDIR)"
	@set -e; for file in $(DISTFILES); do \
		test -f "$$file" || { printf 'Missing distribution file: %s\n' "$$file" >&2; exit 1; }; \
	done
	mkdir "$(DISTDIR)"
	@set -e; for file in $(DISTFILES); do mkdir -p "$(DISTDIR)/$$(dirname "$$file")"; cp "$$file" "$(DISTDIR)/$$file"; done
	tar -czf "$(DISTDIR).tar.gz" "$(DISTDIR)"
	rm -r "$(DISTDIR)"

clean:
	rm -f $(TARGET) fluid build_config.h build_config.h.tmp program_defaults.h program_defaults.h.tmp
