CC=g++
CFLAGS=-Wall -Iincludes -Wextra -ggdb -std=c++17 -g
LDLIBS=-lcrypto -lpthread
VPATH=src

all: client

client: client.o hash.o client.cpp
	g++ client.o hash.o ${CFLAGS} ${LDLIBS} -o client

client.o:
	g++ -c client.cpp ${CFLAGS} ${LDLIBS}

hash.o:
	g++ -c deps/hash.c ${CFLAGS} ${LDLIBS}

clean:
	rm -rf *~ *.o client

.PHONY : clean all
