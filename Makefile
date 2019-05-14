all: server client

%.o : %.c uftp.h
	gcc -g -c $< -o $@

clean:
	rm *.o

server: server.o sha1.o
	gcc -g $^ -o server -lpthread -lrt

client: client.o sha1.o
	gcc -g $^ -o client

.PHONY: clean