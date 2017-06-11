CPPFLAGS=-std=c++14 -Wall -O3

all: siktacka-server siktacka-client

siktacka-server: siktacka.h util.h server.cpp
	g++ $(CPPFLAGS) server.cpp -lz -o siktacka-server

siktacka-client: siktacka.h util.h client.cpp
	g++ $(CPPFLAGS) client.cpp -lz -o siktacka-client

.PHONY: clean
clean:
	rm -f siktacka-server siktacka-client