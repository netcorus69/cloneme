CC = gcc

# Compiler flags
CFLAGS = -Wall -Wextra -O2 -I./vosk-local $(shell pkg-config --cflags gtk+-3.0)

# Linker flags — lportaudio removed, using arecord pipe instead
LDFLAGS = -lSDL2 -lpthread -lportaudio -L./vosk-local -lvosk $(shell pkg-config --libs gtk+-3.0)

# Object files
OBJS = cloneme.o clone.o memory.o voice.o face.o mic.o respond.o

# Header dependencies — if tee_log.h changes, recompile all affected .o files
HEADERS = tee_log.h

# Default target (release build)
cloneme: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# Compile rules — any .o depends on its .c AND shared headers
%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

# Debug build
debug: CFLAGS = -Wall -Wextra -g -O0 -DDEBUG -I./vosk-local $(shell pkg-config --cflags gtk+-3.0)
debug: $(OBJS)
	$(CC) $(CFLAGS) -o cloneme $(OBJS) $(LDFLAGS)

# Run target
run: cloneme
	PULSE_LATENCY_MSEC=30 LD_LIBRARY_PATH=./vosk-local ./cloneme

# Full rebuild from scratch
rebuild: clean cloneme

# Clean up
clean:
	rm -f $(OBJS) cloneme
