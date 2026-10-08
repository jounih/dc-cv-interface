# Host unit tests for the pure firmware modules (no Pico SDK needed).
#   make -C firmware/test        # 4-out and 8-out builds
SRC = ../src/cal.c ../src/engine.c ../src/proto.c ../src/usb_desc.c test_host.c
CFLAGS = -std=c11 -O1 -Wall -Wextra -Werror -Wno-missing-field-initializers -I../src -fsanitize=address,undefined

all: test4 test8
	./test4
	./test8

test4: $(SRC) ../src/*.h
	$(CC) $(CFLAGS) -DCV_N_OUT=4 -o $@ $(SRC) -lm

test8: $(SRC) ../src/*.h
	$(CC) $(CFLAGS) -DCV_N_OUT=8 -o $@ $(SRC) -lm

clean:
	rm -f test4 test8

.PHONY: all clean
