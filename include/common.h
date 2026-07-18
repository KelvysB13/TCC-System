#pragma once
#include <windows.h>

/*
 * common.h - Constantes y tipos compartidos entre todos los módulos
 * del sistema de telemetría. Define el protocolo de datos, los nombres
 * de los objetos del kernel de Windows y las estructuras de memoria
 * compartida.
 */

/* Tamaño máximo de la carga útil de un evento de telemetría */
#define MAX_PAYLOAD_SIZE      256

/* Capacidad del buffer circular en número de eventos */
#define CIRCULAR_BUFFER_CAP   1024

/* Máximo de sensores simultáneos soportados */
#define MAX_SENSORS           64

/* Tamaño del buffer interno del Named Pipe (4 KB) */
#define PIPE_BUFFER_SIZE      4096

/* Nombre del Named Pipe para la comunicación sensores → broker */
#define PIPE_NAME             "\\\\.\\pipe\\TelemetryBrokerPipe"

/* Nombre del objeto de mapeo de archivos (memoria compartida) */
#define SHM_NAME              "Local\\TelemetrySharedMemory"

/* Nombre del mutex para acceso exclusivo al buffer circular */
#define MUTEX_NAME            "Local\\TelemetryBufferMutex"

/*
 * Semáforo que cuenta los slots vacíos del buffer circular.
 * Cuando llega a 0, el buffer está lleno → backpressure.
 */
#define SEM_EMPTY_NAME        "Local\\TelemetrySemEmpty"

/*
 * Semáforo que cuenta los slots ocupados del buffer circular.
 * Cuando llega a 0, el buffer está vacío.
 */
#define SEM_FULL_NAME         "Local\\TelemetrySemFull"

/* Evento de señalización para shutdown coordinado entre procesos */
#define SHUTDOWN_EVENT_NAME   "Local\\TelemetryShutdownEvent"

/* Tipos de sensores físicos simulados */
typedef enum {
    SENSOR_MOTOR   = 1,   /* Sensor del motor */
    SENSOR_TIRES   = 2,   /* Sensor de neumáticos */
    SENSOR_BRAKES  = 3,   /* Sensor de frenos */
    SENSOR_GPS     = 4    /* Sensor de telemetría GPS */
} SensorType;

/*
 * Estructura empaquetada de evento de telemetría.
 * #pragma pack(1) asegura que no haya padding entre campos,
 * tal como exige la consigna ("estructuras de datos empaquetadas").
 * Esto garantiza que el tamaño en bytes sea exactamente predecible
 * para la transmisión por Named Pipe.
 */
#pragma pack(push, 1)
typedef struct {
    DWORD         eventId;          /* ID único asignado por el broker */
    DWORD         sensorId;         /* Tipo de sensor (1-4) */
    DWORD         priority;         /* Prioridad del evento */
    LARGE_INTEGER timestamp;        /* Marca de tiempo de alta resolución (QueryPerformanceCounter) */
    BYTE          payload[MAX_PAYLOAD_SIZE]; /* Carga útil de datos del sensor */
    DWORD         payloadSize;      /* Tamaño real de los datos en payload */
} TelemetryEvent;
#pragma pack(pop)

/*
 * Buffer circular alojado en memoria compartida (File Mapping).
 * Esta estructura es visible para:
 *   - Módulo 2 (Broker):  escribe eventos entrantes
 *   - Módulo 3 (Dispatcher): lee eventos para distribuirlos a Workers
 *   - Módulo 4 (Monitor):  lee estadísticas en tiempo real
 *
 * Los campos volatile LONG son accedidos atómicamente mediante
 * InterlockedIncrement/Decrement y protegidos por el mutex.
 */
typedef struct {
    volatile LONG initialized;         /* 1 si ya fue inicializado */
    volatile LONG eventsProcessed;     /* Contador total de eventos procesados */
    volatile LONG activeSensors;       /* Número de sensores actualmente conectados */
    volatile LONG bufferCount;         /* Cantidad de eventos en el buffer */
    volatile LONG bufferWriteIndex;    /* Índice de escritura (productor) */
    volatile LONG bufferReadIndex;     /* Índice de lectura (consumidor) */
    DWORD         bufferCapacity;      /* Capacidad máxima del buffer (CIRCULAR_BUFFER_CAP) */
    TelemetryEvent buffer[CIRCULAR_BUFFER_CAP]; /* Arreglo circular de eventos */
} SharedCircularBuffer;
