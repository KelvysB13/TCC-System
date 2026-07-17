/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 * Modulo 1  - Subsistema de Sensores (Procesos Independientes)
 *
 * Desarrollador: Samuel Prado
 * C.I:          31.701.746
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
    DWORD        idSensor;    // ID de la instancia del sensor (SENSOR_EVENT_HEADER.dwSensorId)
    DWORD        idEvento;    // Numero de secuencia del evento (SENSOR_EVENT_HEADER.dwEventId)
    SENSOR_TYPE  tipo;        // Tipo de sensor
    Prioridad    prio;        // Prioridad asignada
    ULONGLONG    ts;          // Timestamp en microsegundos (SENSOR_EVENT_HEADER.ullTimestamp)
    int          datos[4];    // Valores del payload procesado
} Evento;

typedef struct {
    Evento       items[CAP_BUF];
    volatile LONG cabeza;
    volatile LONG cola;
    volatile LONG cnt;
} BufCirc;

#ifdef __cplusplus
}
#endif

#endif
