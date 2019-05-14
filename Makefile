all: server client

%.o : %.c uftp.h
	gcc -c $< -o $@


server: server.o sha1.o
	gcc $^ -o server -lpthread -lrt

client: client.o sha1.o
	gcc $^ -o client
