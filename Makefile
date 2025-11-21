# Makefile para cliente FTP Concurrente
CC = gcc
CFLAGS = -Wall -Wextra -O2

OBJS = YarK-clienteFTP.o connectsock.o connectTCP.o \
       passivesock.o passiveTCP.o errexit.o
TARGET = clienteFTP

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET) $(OBJS)