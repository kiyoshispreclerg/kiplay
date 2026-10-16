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
      src/lineio.c \
      src/child.c \
      src/backend.c \
      src/audioback.c \
      src/tagreader.c \
      src/media.c \
      src/search.c \
      src/mpv.c \
      src/mpvopts.c \
      src/filesettings.c \
      src/mpris.c \
      src/ytsub.c \
      src/srv3.c \
      src/json3.c \
      src/sidecar.c \
      src/chatlog.c \
      src/ytsub_ass.c \
      src/ytdlp.c
VENDOR_SRC = vendor/cjson/cJSON.c
LIBKP = $(BUILD)/libkp.a

# The window: needs GTK.
APP_SRC = src/main.c \
      ui/window.c \
      ui/layout.c \
      ui/playlists.c \
      ui/plmodel.c \
      ui/jump.c \
      ui/controls.c \
      ui/fsbar.c \
      ui/player.c \
      ui/shortcuts.c \
      ui/tracks.c \
      ui/subdl.c \
      ui/prefsdlg.c \
      ui/desktop.c \
      ui/imgcache.c \
      ui/chatpanel.c
APP_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(APP_SRC))

# kiplay-audio: the music engine, its own process. No GLib in it: libav,
# ALSA and pthreads only, to stay small.
AUDIO_SRC = audio/main.c audio/cmd.c audio/dec.c audio/game.c audio/out.c audio/tags.c
AUDIO_SRC += $(if $(filter 1,$(PULSE)),audio/pulse.c)
AUDIO_OBJ = $(patsubst %.c,$(BUILD)/%.o,$(AUDIO_SRC))
ENGINE_PKGS = alsa libavformat libavcodec libavutil libswresample zlib $(if $(filter 1,$(PULSE)),libpulse)
ENGINE_CFLAGS = $(shell $(PKG_CONFIG) --cflags $(ENGINE_PKGS) | sed 's/-I/-isystem /g') -isystem vendor/gme \
                $(if $(filter 1,$(PULSE)),-DKA_HAVE_PULSE)
ENGINE_LIBS = $(LIBGME) $(shell $(PKG_CONFIG) --libs $(ENGINE_PKGS)) -lpthread -lm

# Game_Music_Emu (C++), ours: see audio/game.h. Hidden symbols, so that the
# system's libgme, which libavformat loads, never meets them. The YM2612 is
# Gens' (GME's default, Nuked, costs some 25 times the CPU). Its counters
# rely on signed overflow wrapping around: -fwrapv makes that defined.
GME_SRC = $(wildcard vendor/gme/*.cpp)
GME_OBJ = $(patsubst %.cpp,$(BUILD)/%.o,$(GME_SRC)) $(BUILD)/vendor/gme/ext/emu2413.o
LIBGME = $(BUILD)/libgme.a
GME_FLAGS = -fvisibility=hidden -fwrapv -Ivendor/gme -DVGM_YM2612_GENS
GME_CXXFLAGS = -std=c++11 -fno-exceptions -fno-rtti -fvisibility-inlines-hidden
# Emulators shift negative samples on purpose: UBSan would stop there.
GME_SANFLAGS = $(SANFLAGS) $(if $(SANFLAGS),-fno-sanitize=shift)

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

all: $(BUILD)/kiplay $(BUILD)/kiplay-audio $(MO_FILES) $(PROTOS)
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

$(BUILD)/kiplay-audio: $(AUDIO_OBJ) $(LIBGME)
	$(CXX) $(ALL_LDFLAGS) -o $@ $(AUDIO_OBJ) $(ENGINE_LIBS)

$(LIBGME): $(GME_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/vendor/gme/%.o: vendor/gme/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(GME_FLAGS) $(GME_CXXFLAGS) $(OPTFLAGS) $(GME_SANFLAGS) $(CXXFLAGS) $(VENDOR_WARNFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/vendor/gme/ext/emu2413.o: vendor/gme/ext/emu2413.c
	@mkdir -p $(@D)
	$(CC) $(GME_FLAGS) $(OPTFLAGS) $(GME_SANFLAGS) $(CFLAGS) $(VENDOR_WARNFLAGS) -MMD -MP -c -o $@ $<

$(AUDIO_OBJ): $(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) -Iaudio $(STDFLAGS) $(OPTFLAGS) $(SANFLAGS) $(CFLAGS) $(ENGINE_CFLAGS) $(WARNFLAGS) -MMD -MP -c -o $@ $<

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

TEST_CPPFLAGS = -DKP_FIXTURES_DIR='"$(CURDIR)/tests/fixtures"' \
                -DKP_AUDIO_BIN='"$(CURDIR)/$(BUILD)/kiplay-audio"'

# test_audio also links the engine's own parts (they aren't in libkp).
TEST_AUDIO_OBJ = $(BUILD)/audio/cmd.o $(BUILD)/audio/dec.o $(BUILD)/audio/game.o $(BUILD)/audio/tags.o
$(BUILD)/tests/test_audio: TEST_EXTRA = $(TEST_AUDIO_OBJ) $(ENGINE_LIBS) -lstdc++
$(BUILD)/tests/test_audio: TEST_EXTRA_CFLAGS = -Iaudio $(ENGINE_CFLAGS)
$(BUILD)/tests/test_audio: $(TEST_AUDIO_OBJ) $(BUILD)/kiplay-audio

$(BUILD)/tests/%: tests/unit/%.c tests/harness.h $(LIBKP)
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(TEST_CPPFLAGS) $(TEST_EXTRA_CFLAGS) $(ALL_CFLAGS) $(WARNFLAGS) -MMD -MP \
		$(ALL_LDFLAGS) -o $@ $< $(LIBKP) $(TEST_EXTRA) $(LDLIBS)

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

# GTK and Qt trust icon-theme.cache over the folder: without this a new icon
# is invisible until someone rebuilds it. Same for the .desktop MIME cache
# ("Open with"). Skipped when staging into a DESTDIR.
define refresh-caches
	$(if $(DESTDIR),,-gtk-update-icon-cache -f -t $(DATADIR)/icons/hicolor
	-update-desktop-database $(DATADIR)/applications)
endef

install: $(BUILD)/kiplay $(BUILD)/kiplay-audio $(MO_FILES)
	install -Dm755 $< $(DESTDIR)$(BINDIR)/kiplay
	install -Dm755 $(BUILD)/kiplay-audio $(DESTDIR)$(BINDIR)/kiplay-audio
	$(foreach d,kiplay kiplay-enqueue kiplay-newlist,install -Dm644 data/$(d).desktop \
		$(DESTDIR)$(DATADIR)/applications/$(d).desktop;)
	install -Dm644 data/kiplay.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/kiplay.svg
	$(foreach l,$(if $(MO_FILES),$(LINGUAS)),install -Dm644 $(BUILD)/locale/$(l)/LC_MESSAGES/kiplay.mo \
		$(DESTDIR)$(LOCALEDIR)/$(l)/LC_MESSAGES/kiplay.mo;)
	$(call refresh-caches)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/kiplay $(DESTDIR)$(BINDIR)/kiplay-audio
	for l in $(LINGUAS); do rm -f $(DESTDIR)$(LOCALEDIR)/$$l/LC_MESSAGES/kiplay.mo; done
	rm -f $(DESTDIR)$(DATADIR)/applications/kiplay.desktop \
	      $(DESTDIR)$(DATADIR)/applications/kiplay-enqueue.desktop \
	      $(DESTDIR)$(DATADIR)/applications/kiplay-newlist.desktop \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/kiplay.svg
	$(call refresh-caches)

lint:
	@if grep -nE '\b(strcpy|strcat|sprintf|vsprintf|gets)[[:space:]]*\(' src/*.c src/*.h ui/*.c ui/*.h audio/*.c audio/*.h proto/*.c tests/unit/*.c; then \
		echo "*** banned function used"; exit 1; fi
	@command -v cppcheck >/dev/null || { echo "*** cppcheck not installed"; exit 1; }
	cppcheck --std=c11 --enable=warning,performance,portability \
		--error-exitcode=1 --inline-suppr --quiet -Isrc -Iaudio -Ivendor/cjson --suppress='*:vendor/*' \
		-D_XOPEN_SOURCE=700 -DKP_VERSION='"lint"' -DKP_FIXTURES_DIR='"."' src ui audio proto tests/unit

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
