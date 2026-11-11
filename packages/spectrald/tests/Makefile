SRC := ../src
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Werror -std=gnu11 -I$(SRC) -fsanitize=address,undefined

test: test_fft
	./test_fft

test_fft: test_fft.c $(SRC)/fft.c $(SRC)/chan.c $(SRC)/util.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -f test_fft

.PHONY: test clean
