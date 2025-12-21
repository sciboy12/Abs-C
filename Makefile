CC = gcc
LDFLAGS = -linih -lcap -lcjson -lwebsockets
TARGET = abs-c
SOURCES = abs-c.c tosuhandler.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) -o $(TARGET) $(SOURCES) $(LDFLAGS)

clean:
	$(RM) $(TARGET)

