# Makefile for nfzf (termios version)

CC := clang
STRIP := strip

SRC := src/main.c
TARGET := nf
BINDIR := bin

CFLAGS := -Wall -Wextra -std=c99
LDFLAGS :=

CFLAGS_DEBUG := -g -O0
CFLAGS_RELEASE := -O2
CFLAGS_SIZE := -Oz -flto -ffunction-sections -fdata-sections
CFLAGS_TINY := -Oz -flto -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables

UNAME := $(shell uname)
ifeq ($(UNAME), Darwin)
LDFLAGS += -dead_strip
else ifeq ($(UNAME), Linux)
LDFLAGS += -Wl,--gc-sections
endif

.PHONY: all
all: release

.PHONY: debug
debug: CFLAGS += $(CFLAGS_DEBUG)
debug: $(TARGET)_debug

$(TARGET)_debug: $(SRC)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(SRC) -o $(BINDIR)/$@ $(LDFLAGS)
	@echo "Built debug: $(BINDIR)/$@"
	@ls -lh $(BINDIR)/$@

.PHONY: release
release: CFLAGS += $(CFLAGS_RELEASE)
release: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(SRC) -o $(BINDIR)/$@ $(LDFLAGS)
	@echo "Built release: $(BINDIR)/$@"
	@ls -lh $(BINDIR)/$@

.PHONY: small
small: CFLAGS += $(CFLAGS_SIZE)
small: $(TARGET)_small

$(TARGET)_small: $(SRC)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(SRC) -o $(BINDIR)/$@ $(LDFLAGS)
	@echo "Built small: $(BINDIR)/$@"
	@ls -lh $(BINDIR)/$@

.PHONY: tiny
tiny: CFLAGS += $(CFLAGS_TINY)
tiny: $(TARGET)_tiny

$(TARGET)_tiny: $(SRC)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) $(SRC) -o $(BINDIR)/$@ $(LDFLAGS)
	@echo "Built tiny: $(BINDIR)/$@"
	@ls -lh $(BINDIR)/$@

.PHONY: compare
compare: debug release small tiny
	@echo ""
	@echo "=== Size Comparison ==="
	@ls -lh $(BINDIR)/$(TARGET)* | awk '{print $$5 "\t" $$9}'

.PHONY: clean
clean:
	rm -rf $(BINDIR)/$(TARGET)*
	@echo "Cleaned"

.PHONY: help
help:
	@echo "Targets:"
	@echo "  all (default) - Build release"
	@echo "  debug         - Build with -g"
	@echo "  release       - Build with -O2"
	@echo "  small         - Build with -Oz"
	@echo "  tiny          - Build with max optimization"
	@echo "  compare       - Build all and compare sizes"
	@echo "  clean         - Remove builds"