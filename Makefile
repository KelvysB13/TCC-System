CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -std=c99
LDFLAGS  =
INCLUDES = -Iinclude

SENSOR_SRC     = src/sensors/sensor.c
SENSOR_OUT     = bin/sensor.exe

DISPATCHER_SRC = src/dispatcher/dispatcher.c src/dispatcher/buffer.c src/dispatcher/processor.c
DISPATCHER_OUT = bin/dispatcher.exe

.PHONY: All sensor dispatcher clean run

All: sensor dispatcher

sensor:
	$(CC) $(CFLAGS) $(INCLUDES) $(SENSOR_SRC) -o $(SENSOR_OUT) $(LDFLAGS)

dispatcher:
	$(CC) $(CFLAGS) $(INCLUDES) $(DISPATCHER_SRC) -o $(DISPATCHER_OUT) $(LDFLAGS)

run: dispatcher
	$(DISPATCHER_OUT)

clean:
	-del /Q bin\*.exe 2>nul
