CC = gcc
LDFLAGS = -linih
TARGET = abs-c

all: $(TARGET) 
	$(CC) -o $(TARGET) $(TARGET).c $(LDFLAGS)
clean:
	$(RM) $(TARGET)
