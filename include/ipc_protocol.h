/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 * Modulo 1  - Subsistema de Sensores (Procesos Independientes)
 *
 * Desarrollador: Samuel Prado
 * C.I:          31.701.746
 *
 * ipc_protocol.h - Constantes del protocolo de comunicacion IPC
 *                  entre modulos via Named Pipes.
 */

#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

#define TCC_PIPE_PREFIX             "\\\\.\\pipe\\TCC_Sensor_"
#define TCC_PIPE_BUFFER_SIZE        (64 * 1024)
#define TCC_PIPE_TIMEOUT            5000
#define TCC_PIPE_WAIT_INTERVAL      1000
#define TCC_PIPE_RETRY_COUNT        30

#define TCC_SENSOR_PAYLOAD_ENGINE   48
#define TCC_SENSOR_PAYLOAD_TIRES    36
#define TCC_SENSOR_PAYLOAD_BRAKES   24
#define TCC_SENSOR_PAYLOAD_GPS      40

#define TCC_MAX_PAYLOAD_SIZE        64

#define TCC_DEFAULT_INTERVAL_MS     100

#endif
