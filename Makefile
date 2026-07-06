#
# TCC-System - Sistema de Telemetria y Control Concurrente
# Modulo 1  - Subsistema de Sensores (Procesos Independientes)
#
# Desarrollador: Samuel Prado
# C.I:          31.701.746
#

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -std=c99
LDFLAGS  =
INCLUDES = -Iinclude

SENSOR_SRC  = src/sensors/sensor.c
SENSOR_OUT  = bin/sensor.exe

.PHONY: All sensor clean

All: sensor

sensor:
	$(CC) $(CFLAGS) $(INCLUDES) $(SENSOR_SRC) -o $(SENSOR_OUT) $(LDFLAGS)

clean:
	-del /Q bin\*.exe 2>nul
