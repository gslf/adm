CFLAGS = -std=c11 -Wall -Wextra -O2 -D_DEFAULT_SOURCE
SRC = $(wildcard src/*.c)
HDR = $(wildcard src/*.h)

adm: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	$(RM) adm
