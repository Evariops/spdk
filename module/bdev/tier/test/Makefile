# T1: host-compilable unit tests of the PRODUCTION tier code (see test_tier_sb.c).
# No SPDK build required — mock headers in mock/spdk. ASAN+UBSAN always on.

CC      ?= cc
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function
CFLAGS  += -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS  += -Imock -I..

BIN := test_tier_sb

.PHONY: all run clean

all: run

$(BIN): test_tier_sb.c ../vbdev_tier_sb.c ../vbdev_tier.h $(wildcard mock/spdk/*.h)
	$(CC) $(CFLAGS) -o $@ test_tier_sb.c

run: $(BIN)
	./$(BIN)

clean:
	rm -rf $(BIN) $(BIN).dSYM
