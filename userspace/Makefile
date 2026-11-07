# SPDX-License-Identifier: GPL-2.0-or-later
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDLIBS += -lpthread
SRCS = main.c mz0380.c mst3367.c it6612.c stream.c v4l2out.c vfio.c
OBJS = $(SRCS:.c=.o)

hd60pro: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c vfio.h mz0380.h mst3367.h it6612.h it6612_tables.h stream.h v4l2out.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f hd60pro $(OBJS)

.PHONY: clean
