CC=gcc 
CFLAGS=-std=gnu99 -Wall -Wextra -Werror -pedantic

all:
	$(CC) $(CFLAGS) -o proj2 proj2.c -lpthread