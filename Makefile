include config.mk

BUILD ?= build

# Everything in src/ goes into a static lib the unit tests and the
# prototypes link against.
LIB_SRC = src/ytsub.c \
      src/srv3.c \
      src/ytsub_ass.c
LIBKP = $(BUILD)/libkp.a

# Phase 0 prototypes (proto/*.c, one program each).
PROTOS = $(BUILD)/proto/subconv $(BUILD)/proto/embed $(BUILD)/proto/audio

UNIT_TESTS = $(patsubst tests/unit/%.c,$(BUILD)/tests/%,$(wildcard tests/unit/test_*.c))

ALL_CFLAGS  = $(STDFLAGS) $(OPTFLAGS) $(SANFLAGS) $(PKG_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(SANFLAGS) $(LDFLAGS)

.PHONY: all test asan lint clean

all: $(LIBKP) $(PROTOS)

$(LIBKP): $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRC))
	$(AR) rcs $@ $^

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

asan:
	$(MAKE) BUILD=$(BUILD)/asan SANITIZE=1 test

lint:
	@if grep -nE '\b(strcpy|strcat|sprintf|vsprintf|gets)[[:space:]]*\(' src/*.c src/*.h proto/*.c tests/unit/*.c; then \
		echo "*** banned function used"; exit 1; fi
	@command -v cppcheck >/dev/null || { echo "*** cppcheck not installed"; exit 1; }
	cppcheck --std=c11 --enable=warning,performance,portability \
		--error-exitcode=1 --inline-suppr --quiet -Isrc --suppress='*:vendor/*' \
		-D_XOPEN_SOURCE=700 -DKP_VERSION='"lint"' -DKP_FIXTURES_DIR='"."' src proto tests/unit

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
