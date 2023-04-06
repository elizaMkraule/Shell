# Makefile for the CS:APP Shell Lab

DRIVER = ./sdriver.pl
TSH = ./tsh
TSHREF = ./tshref
TSHARGS = "-p"
CC = cc
CFLAGS = -std=gnu11 -Werror -Wall -Wextra -O2 -g
FILES = $(TSH) ./myspin ./mysplit ./mystop ./myint

all: $(FILES)

$(TSH): tsh.o
	$(CC) $(CFLAGS) -o $(TSH) tsh.o

tsh.o: tsh.c

# clean up
clean:
	$(RM) $(FILES) *.o *~ core.[1-9]*

