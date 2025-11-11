CC = gcc
CFLAGS = 
LDFLAGS = -lgpiod
TARGET = fishfeeder
SRC = main.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
