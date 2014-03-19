CC	= gcc
CFLAGS	= -Wall -Wextra -O2
LIBS	= -lm -lpthread -lrt

all: test_sleep

test_sleep: test_sleep.o
	$(CC) -o $@ $(filter %.o,$^) $(LIBS)

clean:
	rm -rf test_sleep *.o
