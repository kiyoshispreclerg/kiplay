include config.mk

BUILD ?= build

# Everything in src/ goes into a static lib the unit tests and the
# prototypes link against.
LIB_SRC = src/log.c \
      src/util.c \
      src/ini.c \
      src/prefs.c \
      src/playlist.c \
      src/plset.c \
      src/instance.c \
      src/json.c \
      src/mpv.c \
      src/ytsub.c \
      src/srv3.c \
      src/ytsub_ass.c
VENDOR_SRC = vendor/cjson/cJSON.c
LIBKP = $(BUILD)/libkp.a

# The window: needs GTK.
APP_SRC = src/main.c \
      ui/window.c \
      ui/layout.c \
      ui/playlists.c \
      ui/controls.c \
      ui/player.c
APP_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(APP_SRC))

# Phase 0 prototypes (proto/*.c, one program each).
PROTOS = $(BUILD)/proto/subconv $(BUILD)/proto/embed $(BUILD)/proto/audio

UNIT_TESTS = $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(wildcard tests/unit/test_*.c))

ALL_CFLAGS  = $(STDFLAGS) $(OPTFLAGS) $(SANFLAGS) $(PKG_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(SANFLAGS) $(LDFLAGS)

.PHONY: all test run asan asan-run install uninstall lint clean locale pot update-po

# Translations (po/LINGUAS lists the languages). Built into $(BUILD)/locale,
# which the program finds next to its own executable.
LINGUAS = $(shell sed 's/\#.*//' po/LINGUAS)
HAVE_MSGFMT := $(shell command -v msgfmt >/dev/null 2>&1 && echo 1)
MO_FILES = $(if $(HAVE_MSGFMT),$(foreach l,$(LINGUAS),$(BUILD)/locale/$(l)/LC_MESSAGES/kiplay.mo))

all: $(BUILD)/kiplay $(MO_FILES) $(PROTOS)
locale: $(MO_FILES)

$(BUILD)/locale/%/LC_MESSAGES/kiplay.mo: po/%.po
	@mkdir -p $(@D)
	msgfmt -c -o $@ $<

# Rebuilds the template from the sources and merges it into every .po. No
# --package-version: config.mk's VERSION is the only place the version
# lives, so bumping it doesn't touch the .po files too.
pot update-po:
	xgettext --from-code=UTF-8 -L C --keyword=_ --keyword=N_ --keyword=ngettext:1,2 \
		--add-comments=TRANSLATORS -f po/POTFILES.in -o po/kiplay.pot \
		--package-name=kiplay \
		--copyright-holder="Kiplay contributors" --msgid-bugs-address=""
	for l in $(LINGUAS); do msgmerge -q -U --backup=none po/$$l.po po/kiplay.pot; done

$(BUILD)/kiplay: $(APP_OBJ) $(LIBKP)
	$(CC) $(ALL_LDFLAGS) -o $@ $^ $(GTK_LIBS) $(LDLIBS)

$(APP_OBJ): $(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -Iui -DLOCALEDIR='"$(LOCALEDIR)"' $(ALL_CFLAGS) $(GTK_CFLAGS) $(WARNFLAGS) -MMD -MP -c -o $@ $<

$(LIBKP): $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRC) $(VENDOR_SRC))
	$(AR) rcs $@ $^

$(BUILD)/vendor/%.o: vendor/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(VENDOR_WARNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/proto/subconv: proto/subconv.c $(LIBKP)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -MMD -MP $(ALL_LDFLAGS) -o $@ $< $(LIBKP) $(LDLIBS)

$(BUILD)/proto/embed: proto/embed.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(GTK_CFLAGS) $(WARNFLAGS) -MMD -MP $(ALL_LDFLAGS) -o $@ $< $(GTK_LIBS) $(LDLIBS)

AUDIO_PKGS = alsa libavformat libavcodec libavutil libswresample
$(BUILD)/proto/audio: proto/audio.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) $(shell $(PKG_CONFIG) --cflags $(AUDIO_PKGS) | sed 's/-I/-isystem /g') \
		$(WARNFLAGS) -Wno-missing-prototypes -Wno-unused-function -MMD -MP $(ALL_LDFLAGS) -o $@ $< \
		$(shell $(PKG_CONFIG) --libs $(AUDIO_PKGS)) -lm

# ---- tests ----------------------------------------------------------------

TEST_CPPFLAGS = -DKP_FIXTURES_DIR='"$(CURDIR)/tests/fixtures"'

$(BUILD)/tests/%: tests/unit/%.c tests/harness.h $(LIBKP)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -MMD -MP \
		$(ALL_LDFLAGS) -o $@ $< $(LIBKP) $(LDLIBS)

test: all $(UNIT_TESTS)
	@fail=0; for t in $(UNIT_TESTS); do \
		$$t || fail=1; \
	done; \
	if [ $$fail -ne 0 ]; then echo "*** unit tests FAILED"; exit 1; fi; \
	echo "*** all unit tests passed"

run: $(BUILD)/kiplay $(MO_FILES)
	$(BUILD)/kiplay $(ARGS)

asan:
	LSAN_OPTIONS=suppressions=$(CURDIR)/tools/lsan.supp \
		$(MAKE) BUILD=$(BUILD)/asan SANITIZE=1 test

asan-run:
	$(MAKE) BUILD=$(BUILD)/asan SANITIZE=1 $(BUILD)/asan/kiplay
	# Full stacks on malloc, so the GTK suppressions can match.
	ASAN_OPTIONS=fast_unwind_on_malloc=0 LSAN_OPTIONS=suppressions=$(CURDIR)/tools/lsan.supp \
		$(BUILD)/asan/kiplay $(ARGS)

install: $(BUILD)/kiplay $(MO_FILES)
	install -Dm755 $< $(DESTDIR)$(BINDIR)/kiplay
	install -Dm644 data/kiplay.desktop $(DESTDIR)$(DATADIR)/applications/kiplay.desktop
	install -Dm644 data/kiplay.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/kiplay.svg
	$(foreach l,$(if $(MO_FILES),$(LINGUAS)),install -Dm644 $(BUILD)/locale/$(l)/LC_MESSAGES/kiplay.mo \
		$(DESTDIR)$(LOCALEDIR)/$(l)/LC_MESSAGES/kiplay.mo;)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiplay
	for l in $(LINGUAS); do rm -f $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/kiplay.mo; done
	rm -f $(DESTDIR)$(DATADIR)/applications/kiplay.desktop \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/kiplay.svg

lint:
	@if grep -nE '\b(strcpy|strcat|sprintf|vsprintf|gets)[[:space:]]*\(' src/*.c src/*.h ui/*.c ui/*.h proto/*.c tests/unit/*.c; then \
		echo "*** banned function used"; exit 1; fi
	@command -v cppcheck >/dev/null || { echo "*** cppcheck not installed"; exit 1; }
	cppcheck --std=c11 --enable=warning,performance,portability \
		--error-exitcode=1 --inline-suppr --quiet -Isrc -Ivendor/cjson --suppress='*:vendor/*' \
		-D_XOPEN_SOURCE=700 -DKP_VERSION='"lint"' -DKP_FIXTURES_DIR='"."' src ui proto tests/unit

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
