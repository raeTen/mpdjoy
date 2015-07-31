CC = /usr/bin/gcc
CFLAGS = -Wall
LDFLAGS = -L/usr/lib/ -lmpdclient -lconfig

OBJ = mpdjoy.o
BIN = mpdjoy

joystick: $(OBJ)
	$(CC) $(CFLAGS) -o mpdjoy $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $<

.PHONY: clean
clean:
	rm -rf $(BIN) $(OBJ)
