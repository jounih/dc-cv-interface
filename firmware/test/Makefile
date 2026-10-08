# Host unit tests for the pure firmware modules (no ESP-IDF needed).
SRC = $(wildcard ../common/*.c) test_host.c
SRC := $(filter-out ../common/quirk_os_guessing.c,$(SRC))
CFLAGS = -std=c11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers -I../common -fsanitize=address,undefined

all: test_host test_tiera
	./test_host
	./test_tiera

test_host: $(SRC) ../common/*.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

test_tiera: $(SRC) ../common/*.h
	$(CC) $(CFLAGS) -DCV_TIER_A -o $@ $(SRC) -lm

clean:
	rm -f test_host test_tiera

.PHONY: all clean
