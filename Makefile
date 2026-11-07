CC = gcc
CFLAGS = -Wall -Wextra -O2
LDLIBS = -lncursesw -lm

TARGET = fluid
SRC = fluid.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
