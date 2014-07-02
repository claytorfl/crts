CC=g++
CFLAGS=-Wall

all: crts crts1

crts: crts.cpp
	$(CC) $(CFLAGS) crts.cpp -o crts -lm -lliquid -lpthread -lconfig -luhd -lliquidusrp

crts1: crts1.cpp
	$(CC) $(CFLAGS) crts.cpp -o crts1 -lm -lliquid -lpthread -lconfig -luhd -lliquidusrp

clean:
	rm crts
	rm crts1
