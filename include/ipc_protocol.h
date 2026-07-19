/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 *
 * ipc_protocol.h - Constantes del protocolo de comunicacion IPC
 *                  entre modulos via Named Pipes, Memoria Compartida
 *                  y objetos de sincronizacion.
 *
 * Cada constante define un nombre unico para que los 4 modulos
 * puedan encontrarse y comunicarse en tiempo de ejecucion sin
 * necesidad de archivos de configuracion externos.
 *
 * ARQUITECTURA IPC:
 *   M1 (Sensores) ──Named Pipes──> M2 (Broker)
 *   M2 (Broker)   ──File Mapping + Sem/Mutex──> M3 (Dispatcher)
 *   M4 (Monitor)  ──File Mapping (lectura) + Eventos──> M2 (Broker)
 */

#ifndef IPC_PROTOCOL_H
#define IPC_PROTOCOL_H

/*══════════════════════════════════════════════════════════════════
 *  NAMED PIPES (Modulo 1 <-> Modulo 2)
 *
 *  Los sensores (M1) se conectan al Broker (M2) mediante Named Pipes.
 *  El formato del nombre del pipe es:
 *    \\.\pipe\TCC_Sensor_<Type>_<Id>
 *  donde <Type> es Engine|Tires|Brakes|GPS y <Id> es un numero unico.
 *
 *  Ejemplo: \\.\pipe\TCC_Sensor_Engine_1
 *
 *  El Broker crea los pipes con CreateNamedPipeA y espera conexiones
 *  con ConnectNamedPipe. Los sensores se conectan con CreateFileA.
 *══════════════════════════════════════════════════════════════════*/

#define TCC_PIPE_PREFIX             "\\\\.\\pipe\\TCC_Sensor_"
#define TCC_PIPE_BUFFER_SIZE        (64 * 1024)   /* Buffer interno del pipe (64KB) */
#define TCC_PIPE_TIMEOUT            5000           /* Timeout por defecto (ms) */
#define TCC_PIPE_WAIT_INTERVAL      1000           /* Intervalo entre reintentos (ms) */
#define TCC_PIPE_RETRY_COUNT        30             /* Maximo de reintentos de conexion */

/* Tamanos de payload segun tipo de sensor (definidos por M1) */
#define TCC_SENSOR_PAYLOAD_ENGINE   48
#define TCC_SENSOR_PAYLOAD_TIRES    36
#define TCC_SENSOR_PAYLOAD_BRAKES   24
#define TCC_SENSOR_PAYLOAD_GPS      40

#define TCC_MAX_PAYLOAD_SIZE        64             /* Payload maximo (para buffers fijos) */
#define TCC_DEFAULT_INTERVAL_MS     100            /* Intervalo por defecto entre eventos */

/*══════════════════════════════════════════════════════════════════
 *  MEMORIA COMPARTIDA (Modulo 2 <-> Modulo 3 <-> Modulo 4)
 *
 *  El buffer circular de eventos se aloja en un File Mapping de
 *  Windows con nombre "TCC_SharedBuffer".
 *
 *  M2 (Broker):  CreateFileMappingA + MapViewOfFile (escritura)
 *  M3 (Dispatcher):  OpenFileMappingA + MapViewOfFile (lectura/escritura)
 *  M4 (Monitor): OpenFileMappingA + MapViewOfFile (solo lectura)
 *
 *  La estructura completa esta definida en common.h como SHARED_BUFFER.
 *══════════════════════════════════════════════════════════════════*/

#define TCC_SHARED_MEM_NAME         "TCC_SharedBuffer"
#define TCC_SHARED_MEM_SIZE         (sizeof(SHARED_BUFFER) + 4096)  /* Tamano con margen */

/*══════════════════════════════════════════════════════════════════
 *  SINCRONIZACION IPC (Objetos con nombre)
 *
 *  Todos los objetos de sincronizacion tienen nombre para que
 *  procesos diferentes (Broker, Dispatcher, Monitor) puedan
 *  acceder a los mismos semaforos y mutex.
 *
 *  SEM_EMPTY: Contador de slots libres en el buffer circular.
 *    - Broker (M2): WaitForSingleObject antes de escribir
 *    - Dispatcher (M3): ReleaseSemaphore despues de leer
 *    - Valor inicial: CAP_BUF (64)
 *
 *  SEM_FULL: Contador de slots ocupados (eventos disponibles).
 *    - Broker (M2): ReleaseSemaphore despues de escribir
 *    - Dispatcher (M3): WaitForSingleObject antes de leer
 *    - Valor inicial: 0
 *
 *  MUTEX: Exclusion mutua para operaciones de lectura/escritura
 *    en el buffer circular.
 *    - Todos los accesos a cabeza/cola/items deben estar
 *      protegidos por este mutex
 *══════════════════════════════════════════════════════════════════*/

#define TCC_SEM_EMPTY_NAME          "TCC_BufEmpty"   /* Semaforo: slots libres */
#define TCC_SEM_FULL_NAME           "TCC_BufFull"    /* Semaforo: slots ocupados */
#define TCC_MUTEX_NAME              "TCC_BufMutex"   /* Mutex: exclusion mutua */

/*══════════════════════════════════════════════════════════════════
 *  EVENTOS DE COORDINACION (Modulo 4 <-> Modulo 2)
 *
 *  El Monitor (M4) utiliza Eventos de Windows para coordinar
 *  acciones con el Broker (M2).
 *
 *  SHUTDOWN_EVENT: Senal de apagado controlado.
 *    - M4: SetEvent para iniciar apagado
 *    - M2: Detecta en main loop y llama a Broker_Detener()
 *
 *  DEBUG_EVENT: Toggle de dump de depuracion en consola.
 *    - M4: SetEvent para activar/desactivar
 *    - M2: Alterna el flag g_bDebugDump en reader threads
 *══════════════════════════════════════════════════════════════════*/

#define TCC_EVT_SHUTDOWN_NAME       "TCC_ShutdownEvent"   /* Apagado ordenado */
#define TCC_EVT_DEBUG_NAME          "TCC_DebugEvent"      /* Toggle debug dump */

/*══════════════════════════════════════════════════════════════════
 *  CONFIGURACION DEL BROKER
 *══════════════════════════════════════════════════════════════════*/

/* Numero de instancias de Named Pipe por tipo de sensor.
 * Con 4 tipos de sensor y 2 instancias cada uno = 8 pipes total. */
#define TCC_BROKER_DEFAULT_INSTANCES_PER_TYPE    2
#define TCC_SENSOR_TYPE_COUNT                    4  /* Engine, Tires, Brakes, GPS */

#endif
