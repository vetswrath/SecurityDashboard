CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -I../../main

all: test_ble_ibeacon
	./test_ble_ibeacon

test_ble_ibeacon: test_ble_ibeacon.c ../../main/ble_ibeacon.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f test_ble_ibeacon
