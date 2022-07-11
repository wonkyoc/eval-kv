CC=gcc

all: server client

server: server.c
	$(CC) -g -o $@ $^

client: client.c
	$(CC) -g -o $@ $^

.PHONY: clean


clean:
	rm -rf client server
