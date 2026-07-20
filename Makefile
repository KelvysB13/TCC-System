CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -std=c99
LDFLAGS  =
INCLUDES = -Iinclude

SENSOR_SRC     = src/sensors/sensor.c
SENSOR_OUT     = bin/sensor.exe

BROKER_SRC     = src/broker/broker.c
BROKER_OUT     = bin/broker.exe

DISPATCHER_SRC = src/dispatcher/dispatcher.c src/dispatcher/buffer.c src/dispatcher/processor.c
DISPATCHER_OUT = bin/dispatcher.exe

MONITOR_SRC    = src/monitor/monitor.c
MONITOR_OUT    = bin/monitor.exe

.PHONY: All sensor broker dispatcher monitor clean run

All: sensor broker dispatcher monitor

sensor:
	$(CC) $(CFLAGS) $(INCLUDES) $(SENSOR_SRC) -o $(SENSOR_OUT) $(LDFLAGS)

broker:
	$(CC) $(CFLAGS) $(INCLUDES) $(BROKER_SRC) -o $(BROKER_OUT) $(LDFLAGS)

dispatcher:
	$(CC) $(CFLAGS) $(INCLUDES) $(DISPATCHER_SRC) -o $(DISPATCHER_OUT) $(LDFLAGS)

monitor:
	$(CC) $(CFLAGS) $(INCLUDES) $(MONITOR_SRC) -o $(MONITOR_OUT) $(LDFLAGS)

run: broker
	$(BROKER_OUT)

clean:
	-del /Q bin\*.exe 2>nul

rebuild: clean All
