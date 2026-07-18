#pragma once
#include "common.h"

/*
 * ipc_protocol.h - Protocolo de comunicación entre procesos.
 * Define el mensaje de handshake que el sensor envía al broker
 * al establecer la conexión por Named Pipe.
 */

/*
 * Mensaje de handshake inicial.
 * El sensor lo envía apenas conecta para negociar la versión
 * del protocolo y conocer la capacidad del buffer.
 */
typedef struct {
    DWORD pipeInstances;     /* Número de instancias del pipe */
    DWORD bufferCapacity;    /* Capacidad del buffer circular */
    DWORD shmSize;           /* Tamaño de la memoria compartida */
    DWORD protocolVersion;   /* Versión del protocolo */
} HandshakeMessage;

/* Versión actual del protocolo */
#define PROTOCOL_VERSION 1
