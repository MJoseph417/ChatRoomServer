CC = g++
CFLAGS = -g -Wall -std=c++11

default: server

server: server.cpp
	$(CC) $(CFLAGS) -o server server.cpp

clean: 
	rm server 
	rm -r *.dSYM