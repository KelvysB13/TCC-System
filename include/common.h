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

#ifdef __cplusplus
}
#endif

#endif
