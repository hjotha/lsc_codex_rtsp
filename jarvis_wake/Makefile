CC ?= gcc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -pedantic
LDLIBS ?= -lpthread -lm

TARGET := jarvis_wake
SRC := jarvis_wake.c
REPO_ROOT := ..
OUT_DIR := $(REPO_ROOT)/out
ANYKA_TARGET := $(OUT_DIR)/jarvis_wake_arm

.PHONY: all clean anyka

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

anyka:
	bash "$(REPO_ROOT)/scripts/build_jarvis_wake_anyka.sh"

clean:
	rm -f "$(TARGET)" "$(ANYKA_TARGET)"
