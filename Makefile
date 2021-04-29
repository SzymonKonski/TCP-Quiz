all: client server

client: client.c
	gcc -Wall -o client  client.c

server: server.c
	gcc -Wall -o server server.c

.PHONY: clean all
clean:
	rm  server client