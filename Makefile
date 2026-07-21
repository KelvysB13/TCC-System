CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -std=c99
LDFLAGS  =
INCLUDES = -Iinclude
SHELL = cmd.exe

SENSOR_SRC     = src/sensors/sensor.c
SENSOR_OUT     = bin/sensor.exe

BROKER_SRC     = src/broker/broker.c
BROKER_OUT     = bin/broker.exe

DISPATCHER_SRC = src/dispatcher/dispatcher.c src/dispatcher/buffer.c src/dispatcher/processor.c
DISPATCHER_OUT = bin/dispatcher.exe

MONITOR_SRC    = src/monitor/monitor.c
MONITOR_OUT    = bin/monitor.exe

.PHONY: All sensor broker dispatcher monitor clean run run-all

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

run-all: All
	@echo ============================================
	@echo  Iniciando TCC-System
	@echo ============================================
	@echo [1/4] Iniciando Broker (M2)...
	@start "TCC-Broker" "$(BROKER_OUT)"
	@timeout /t 3 /nobreak > nul
	@echo [2/4] Iniciando Dispatcher (M3) y Monitor (M4)...
	@start "TCC-Dispatcher" "$(DISPATCHER_OUT)"
	@start "TCC-Monitor" "$(MONITOR_OUT)"
	@timeout /t 1 /nobreak > nul
	@echo [3/4] Iniciando Sensores (M1)...
	@start "TCC-Engine-1" "$(SENSOR_OUT)" engine 1
	@start "TCC-Engine-2" "$(SENSOR_OUT)" engine 2
	@start "TCC-Tires-1" "$(SENSOR_OUT)" tires 1
	@start "TCC-Tires-2" "$(SENSOR_OUT)" tires 2
	@start "TCC-Brakes-1" "$(SENSOR_OUT)" brakes 1
	@start "TCC-Brakes-2" "$(SENSOR_OUT)" brakes 2
	@start "TCC-GPS-1" "$(SENSOR_OUT)" gps 1
	@start "TCC-GPS-2" "$(SENSOR_OUT)" gps 2
	@echo [4/4] Sistema iniciado.
	@echo Los logs se guardan en: logs\dispatcher.log
	@echo Para apagar: presiona Ctrl+C en cada ventana.

clean:
	-del /Q bin\*.exe 2>nul
	-if exist logs\dispatcher.log del /Q logs\dispatcher.log

rebuild: clean All
