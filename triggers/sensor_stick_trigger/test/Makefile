# Host tests for the parts of this firmware that are arithmetic rather than I/O.
# No ESP-IDF, no hardware: trigger.c depends on nothing but math.h, which is why
# it is its own file.
#
#   make -C test
#
MAIN   := $(abspath $(CURDIR)/../main)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(MAIN)
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_trigger.c $(MAIN)/trigger.c

.PHONY: all run clean
all: run

test_trigger: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lm

run: test_trigger
	./test_trigger

clean:
	rm -f test_trigger
