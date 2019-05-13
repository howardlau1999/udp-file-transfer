all: server client

%.o : %.c
	gcc -c $< -o $@


server: server.o sha1.o
	gcc $^ -o server -lpthread

client: client.o sha1.o
	gcc $^ -o client