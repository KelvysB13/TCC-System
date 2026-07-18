/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 *
 * Integrantes:
 *   Modulo 1 - Samuel Prado     (C.I: 31.701.746)
 *   Modulo 2 - Rolannys Sanchez (C.I:28.550.912)
 *   Modulo 3 - Kelvys Concepcion
 *   Modulo 4 - 
 *
 * common.h - Tipos, enumeraciones y estructuras compartidas
 *            entre todos los modulos del sistema.
 */

#ifndef COMMON_H
#define COMMON_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Modulo 1: Sensores ──────────────────────────────────── */

typedef enum {
    SENSOR_TYPE_ENGINE = 1,
    SENSOR_TYPE_TIRES  = 2,
    SENSOR_TYPE_BRAKES = 3,
    SENSOR_TYPE_GPS    = 4
} SENSOR_TYPE;

#pragma pack(push, 1)
typedef struct {
    DWORD       dwSensorId;
    DWORD       dwEventId;
    ULONGLONG   ullTimestamp;
    DWORD       dwPayloadSize;
    BYTE        bySensorType;
} SENSOR_EVENT_HEADER;
#pragma pack(pop)

#define SENSOR_EVENT_HEADER_SIZE    sizeof(SENSOR_EVENT_HEADER)

typedef struct {
    DWORD   dwEventsSent;
    DWORD   dwErrors;
    DWORD   dwTotalBytes;
} SENSOR_STATS;

/* ── Modulo 3: Dispatcher ─────────────────────────────────── */

#define CAP_BUF     64
#define CAP_COLA    32
#define N_TRABAJ    4
#define N_PRIOR     4

typedef enum { CRIT, ALTA, NORM, BAJA } Prioridad;

typedef struct {
} Evento;

typedef struct {
    Evento       items[CAP_BUF];
    volatile LONG cabeza;
    volatile LONG cola;
    volatile LONG cnt;
} BufCirc;

/* ── Modulo 2: Broker (Memoria Compartida) ──────────────────
 *
 * SHARED_BUFFER es la estructura central alojada en un File Mapping
 * de Windows. Es visible por:
 *   - Broker (M2): escribe eventos entrantes desde los sensores
 *   - Dispatcher (M3): lee eventos para distribuirlos a workers
 *   - Monitor (M4): lee estadisticas en tiempo real
 *
 * La sincronizacion se realiza mediante objetos named (semafotos +
 * mutex) definidos en ipc_protocol.h.
 *
 * Campos accesibles por M3 (Dispatcher):
 *   - cabeza, cola, cnt: estado del buffer circular
 *   - items[CAP_BUF]:    arreglo de eventos para procesar
 *   - Debe usar los semaforos TCC_SEM_EMPTY_NAME / TCC_SEM_FULL_NAME
 *     y el mutex TCC_MUTEX_NAME para acceso sincronizado
 *
 * Campos accesibles por M4 (Monitor):
 *   - dwEventsReceived:      total de eventos ingestados
 *   - dwActiveSensors:       sensores actualmente conectados
 *   - dwBufferMaxOccupancy:  pico maximo de ocupacion del buffer
 *   - cnt:                   ocupacion actual del buffer
 *   - adwSensorIds/Types:    tabla de sensores conectados
 *   - Solo lectura, no necesita sincronizacion adicional
 *     (lectura de valores volatiles es segura en x86) */

#define MAX_SENSOR_SLOTS    32

typedef struct {
    /* ── Estado del buffer circular (M3 escribe, M2 lee) ─── */
    volatile LONG  cabeza;             /* Indice de proxima escritura */
    volatile LONG  cola;               /* Indice de proxima lectura */
    volatile LONG  cnt;                /* Cantidad de elementos actuales */
    Evento         items[CAP_BUF];     /* Arreglo circular de eventos */

    /* ── Estadisticas en tiempo real para M4 (Monitor) ───── */
    volatile LONG  dwEventsReceived;      /* Total de eventos recibidos */
    volatile LONG  dwActiveSensors;       /* Sensores actualmente conectados */
    volatile LONG  dwBufferMaxOccupancy;  /* Ocupacion maxima historica */

    /* ── Tabla de sensores conectados (para M4) ──────────── */
    DWORD          adwSensorIds[MAX_SENSOR_SLOTS];     /* IDs de sensores por slot */
    DWORD          adwSensorTypes[MAX_SENSOR_SLOTS];   /* Tipos de sensores por slot */
} SHARED_BUFFER;

#ifdef __cplusplus
}
#endif

#endif
