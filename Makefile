CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L \
           -pthread
LDFLAGS = -pthread

TARGET  = rfc2544
SRCS    = main.c packet.c ring.c threads.c test.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c rfc2544.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
