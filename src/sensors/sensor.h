/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 * Modulo 1  - Subsistema de Sensores (Procesos Independientes)
 *
 * Desarrollador: Samuel Prado
 * C.I:          31.701.746
 *
 * sensor.h - Cabecera del modulo sensor. Define la configuracion
 *            de cada instancia y las funciones del ciclo de vida.
 */

#ifndef SENSOR_MODULE_H
#define SENSOR_MODULE_H

#include "../../include/common.h"
#include "../../include/ipc_protocol.h"

typedef struct {
    SENSOR_TYPE eType;
    DWORD       dwId;
    DWORD       dwIntervalMs;
    CHAR        szPipePath[MAX_PATH];
    DWORD       dwPayloadSize;
} SENSOR_CONFIG;

BOOL
Sensor_ParseArgs(
    int argc,
    char* argv[],
    SENSOR_CONFIG* pConfig
);

BOOL
Sensor_ConnectPipe(
    SENSOR_CONFIG* pConfig,
    HANDLE* phPipe
);

BOOL
Sensor_GenerateAndSend(
    SENSOR_CONFIG* pConfig,
    HANDLE hPipe,
    SENSOR_STATS* pStats
);

BOOL
Sensor_GeneratePayload(
    SENSOR_TYPE eType,
    BYTE* pBuffer,
    DWORD dwBufferSize,
    DWORD* pdwSize
);

#endif
