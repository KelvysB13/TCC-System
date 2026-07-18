# Makefile - Sistema de Telemetría y Control Concurrente
#
# Compilador: MinGW GCC 6.3.0 (Windows)
# Flags:      -Wall -Wextra (todas las warnings)
#             -O2 (optimización)
#             -std=c99 (estándar C99)
#
# Solo se requiere windows.h (API Win32 nativa).
# No se usan librerías externas ni frameworks.
#
# Objetivos:
#   all     → compila todos los módulos
#   broker  → compila solo el broker
#   clean   → elimina los ejecutables

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -std=c99
LDFLAGS  =
TARGET   = bin/broker.exe

INCDIR   = -Iinclude

SRC_BROKER = src/broker/broker.c

.PHONY: all clean broker

all: broker

broker:
	$(CC) $(CFLAGS) $(INCDIR) $(SRC_BROKER) -o $(TARGET) $(LDFLAGS)

clean:
	del /Q bin\*.exe 2>nul || exit 0
