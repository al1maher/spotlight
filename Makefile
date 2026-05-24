CC = gcc

CFLAGS = -O2 -Wall -Wextra -std=c11
LIBS = -lX11 -lXrandr -lGL -lGLEW -lm

TARGET = spotlight
SRC = main.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)