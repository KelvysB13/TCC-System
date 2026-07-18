#pragma once
#include "../../include/common.h"
#include "../../include/ipc_protocol.h"

/*
 * broker.h - Interfaz del Módulo 2: Broker Ingestor Central.
 *
 * El Broker es el núcleo del sistema receptor. Es un proceso multihilo que:
 * 1. Escucha conexiones entrantes de sensores via Named Pipe
 * 2. Por cada sensor conectado, crea un hilo receptor dedicado (CreateThread)
 * 3. Cada hilo lee eventos del pipe y los deposita en el buffer circular
 *    en memoria compartida (File Mapping)
 * 4. Coordina el acceso al buffer con semáforos (empty/full) y mutex
 */

/* Inicializa todos los objetos del kernel: File Mapping, Mutex, Semáforos, Evento */
BOOL  Broker_Init(void);

/* Libera todos los recursos y cierra todos los handles del sistema */
void  Broker_Shutdown(void);

/* Bucle principal: acepta conexiones entrantes y lanza hilos receptores */
void  Broker_Run(void);

/* Hilo receptor: lee eventos del Named Pipe y los deposita en el buffer circular */
DWORD WINAPI Broker_ReceiverThread(LPVOID lpParam);

/* Manejador de consola para Ctrl+C, Ctrl+Break y cierre de ventana */
BOOL  WINAPI Broker_ConsoleHandler(DWORD dwCtrlType);
