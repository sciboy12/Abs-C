CC = gcc
LDFLAGS = -lX11 -linih
TARGET = abs-c

all: $(TARGET) 
	$(CC) -o $(TARGET) $(TARGET).c $(LDFLAGS)
clean:
	$(RM) $(TARGET)
