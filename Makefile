# ─────────────────────────────────────────────────────────────────────────────
# Makefile — Tiny Shell (tsh)
#
# Target platform: Linux
# Compiler:        gcc
#
# Primary targets
#   tsh            compile the shell binary (Requirements 1.1, 1.5)
#   test-unit      compile and run all unit tests
#   test-property  compile and run all property-based tests
#   clean          remove all build artefacts
# ─────────────────────────────────────────────────────────────────────────────

CC      = gcc
CFLAGS  = -Wall -Wextra -g -D_GNU_SOURCE
LDFLAGS =

# ── Directories ───────────────────────────────────────────────────────────────
UNIT_DIR  = tests/unit
PROP_DIR  = tests/property
INTEG_DIR = tests/integration

# ── theft PBT library (vendored under tests/theft) ────────────────────────────
THEFT_DIR  = tests/theft
THEFT_INC  = -I$(THEFT_DIR)/inc
THEFT_SRCS = $(wildcard $(THEFT_DIR)/src/*.c)
THEFT_OBJS = $(THEFT_SRCS:.c=.o)

# ── tsh compilation unit used by tests (main suppressed) ─────────────────────
TSH_TEST_FLAGS = $(CFLAGS) -DTESTING $(THEFT_INC) -Itests

# ─────────────────────────────────────────────────────────────────────────────
# tsh — main shell binary
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: tsh
tsh: tsh.c
	$(CC) $(CFLAGS) -o tsh tsh.c $(LDFLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# test-unit — compile and run all unit tests
#
# Unit tests live in tests/unit/*.c.
# Each test file is compiled against tsh.c (with -DTESTING to suppress main).
# A test binary is produced alongside its source file, then executed.
# ─────────────────────────────────────────────────────────────────────────────
UNIT_SRCS = $(wildcard $(UNIT_DIR)/*.c)
UNIT_BINS = $(UNIT_SRCS:.c=)

.PHONY: test-unit
test-unit: $(UNIT_BINS)
	@for bin in $(UNIT_BINS); do \
	    echo "▶  $$bin"; \
	    $$bin || exit 1; \
	done
	@[ -z "$(UNIT_BINS)" ] && echo "(no unit tests yet)" || true

$(UNIT_DIR)/%: $(UNIT_DIR)/%.c tsh.c
	$(CC) $(TSH_TEST_FLAGS) -o $@ $< tsh.c $(LDFLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# test-property — compile and run all property-based tests
#
# Property tests live in tests/property/*.c.
# Each file is compiled against tsh.c (-DTESTING) and the full theft library.
# ─────────────────────────────────────────────────────────────────────────────
PROP_SRCS = $(wildcard $(PROP_DIR)/*.c)
PROP_BINS = $(PROP_SRCS:.c=)

# Compile theft source files (object files placed alongside their sources)
$(THEFT_DIR)/src/%.o: $(THEFT_DIR)/src/%.c
	$(CC) $(CFLAGS) $(THEFT_INC) -I$(THEFT_DIR)/src -c -o $@ $<

.PHONY: test-property
test-property: $(PROP_BINS)
	@for bin in $(PROP_BINS); do \
	    echo "▶  $$bin"; \
	    $$bin || exit 1; \
	done
	@[ -z "$(PROP_BINS)" ] && echo "(no property tests yet)" || true

$(PROP_DIR)/%: $(PROP_DIR)/%.c tsh.c $(THEFT_OBJS)
	$(CC) $(TSH_TEST_FLAGS) -I$(THEFT_DIR)/src \
	      -o $@ $< tsh.c $(THEFT_OBJS) $(LDFLAGS)

# ─────────────────────────────────────────────────────────────────────────────
# clean
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -f tsh
	rm -f $(UNIT_BINS)
	rm -f $(PROP_BINS)
	find $(THEFT_DIR)/src -name "*.o" -delete
	find . -name "*.o" -delete

# ─────────────────────────────────────────────────────────────────────────────
# all (default — just build the shell)
# ─────────────────────────────────────────────────────────────────────────────
.PHONY: all
all: tsh
