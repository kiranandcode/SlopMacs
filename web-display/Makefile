CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?=

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  CFLAGS += -DUSE_EPOLL
endif
ifeq ($(UNAME_S),Darwin)
  CFLAGS += -DUSE_KQUEUE
endif

SRCS = main.c websocket.c
OBJS = $(SRCS:.c=.o)
TARGET = emacs-web-display

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

main.o: main.c websocket.h
websocket.o: websocket.c websocket.h

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
