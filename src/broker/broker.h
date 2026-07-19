/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 * Modulo 2  - Broker Ingestor Central (Proceso Multihilo)
 *
 * Desarrollador: [Tu Nombre]
 * C.I:          [Tu CI]
 *
 * broker.h - Cabecera del modulo Broker Central.
 *
 * Define la estructura PIPE_SLOT que representa cada slot de conexion
 * de sensor, y declara las funciones publicas del ciclo de vida
 * del Broker.
 *
 * Cada PIPE_SLOT contiene:
 *   - szPipeName: nombre del Named Pipe (ej: \\.\pipe\TCC_Sensor_Engine_1)
 *   - hPipe: handle actual del pipe
 *   - hListenerThread: hilo que acepta conexiones entrantes
 *   - hReaderThread: hilo que lee datos del sensor conectado (creado por listener)
 *   - dwSensorType / dwSensorId: identificacion del sensor
 *   - bConnected: flag de conexion activa
 *   - bActive: flag de slot habilitado
 */

#ifndef BROKER_MODULE_H
#define BROKER_MODULE_H

#include "../../include/common.h"
#include "../../include/ipc_protocol.h"

/* Numero maximo de slots = tipos de sensor * instancias por tipo */
#define BROKER_MAX_PIPES    (TCC_SENSOR_TYPE_COUNT * TCC_BROKER_DEFAULT_INSTANCES_PER_TYPE)

/* Estructura de un slot de Named Pipe.
 * Cada slot maneja un nombre de pipe unico y puede tener un
 * listener (esperando conexion) y un reader (leyendo datos). */
typedef struct {
    CHAR   szPipeName[MAX_PATH];   /* Nombre completo del pipe */
    HANDLE hPipe;                  /* Handle del Named Pipe */
    HANDLE hListenerThread;        /* Hilo que acepta conexiones */
    HANDLE hReaderThread;          /* Hilo que lee datos del sensor */
    DWORD  dwSensorType;           /* Tipo de sensor (ENGINE, TIRES, ...) */
    DWORD  dwSensorId;             /* ID unico del sensor */
    BOOL   bConnected;             /* TRUE si hay un sensor conectado */
    BOOL   bActive;                /* TRUE si el slot esta habilitado */
} PIPE_SLOT;

/* Funciones del ciclo de vida del Broker */
BOOL  Broker_Iniciar(void);                    /* Inicializa todo el sistema */
BOOL  Broker_CrearPipes(void);                 /* Crea Named Pipes y listener threads */
DWORD WINAPI Broker_ListenerThread(LPVOID lpParam);  /* Hilo escuchador de conexiones */
DWORD WINAPI Broker_ReaderThread(LPVOID lpParam);    /* Hilo lector de eventos */
BOOL  Broker_BufferEscribir(const Evento* pEvento);  /* Escribe en buffer circular */
void  Broker_BufferInit(void);                      /* Inicializa buffer compartido */
void  Broker_Detener(void);                   /* Detiene todo y libera recursos */
void  Broker_LimpiarSlot(PIPE_SLOT* pSlot);   /* Limpia un slot individual */

#endif
