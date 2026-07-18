/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 * Modulo 2  - Broker Ingestor Central (Proceso Multihilo)
 *
 * Desarrollador: [Tu Nombre]
 * C.I:          [Tu CI]
 *
 * broker.c - Implementacion del Broker Ingestor Central.
 *
 * PROPOSITO:
 *   Nucleo del sistema receptor. Proceso multihilo que gestiona
 *   dinamicamente las conexiones entrantes de los sensores (M1)
 *   via Named Pipes, delega la lectura a hilos dedicados (CreateThread),
 *   y deposita los eventos en un Buffer Circular en Memoria Compartida
 *   (File Mapping) sincronizado con Semafotos y Mutex de Win32.
 *
 * ARQUITECTURA:
 *   main() → Broker_Iniciar()
 *              ├─ Crear File Mapping (memoria compartida)
 *              ├─ Crear Semafotos + Mutex (named para IPC)
 *              ├─ Crear Eventos (shutdown + debug para M4)
 *              └─ Broker_CrearPipes()
 *                   └─ Por cada slot: CreateThread → Broker_ListenerThread
 *                        └─ Bucle: CreateNamedPipe → ConnectNamedPipe
 *                             └─ Conexion → CreateThread → Broker_ReaderThread
 *                                  └─ Bucle: ReadFile → Broker_BufferEscribir
 *
 * SINCRONIZACION:
 *   - Sem_Vacio (CAP_BUF): contador de slots libres en el buffer
 *   - Sem_Lleno (0):       contador de slots ocupados
 *   - Mutex:               exclusion mutua sobre el buffer compartido
 *   - Todos son objetos named para acceso desde M3 (Dispatcher) y M4 (Monitor)
 *
 * BACKPRESSURE:
 *   Si el buffer se llena → Broker_BufferEscribir bloquea en Sem_Vacio
 *   → ReaderThread deja de leer el pipe → buffer del pipe se llena
 *   → WriteFile del sensor se bloquea → backpressure natural sin busy waiting
 */

#define _WIN32_WINNT 0x0601
#define _CRT_SECURE_NO_WARNINGS

#include "broker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* CancelIoEx: declaracion manual para compatibilidad con toolchains antiguos.
 * La funcion existe en kernel32.dll desde Windows Vista, pero algunos
 * headers de MinGW no la exponen con _WIN32_WINNT=0x0601 */
BOOL WINAPI CancelIoEx(HANDLE hFile, LPOVERLAPPED lpOverlapped);

/* ==================================================================
 *  ESTADO GLOBAL DEL BROKER
 *
 *  Todas las variables son static para encapsulamiento dentro del
 *  modulo. Los objetos named (File Mapping, Semafotos, Mutex, Eventos)
 *  son accesibles desde otros procesos a traves de sus nombres unicos
 *  definidos en ipc_protocol.h
 * ================================================================== */

static SHARED_BUFFER*     g_pShared      = NULL; /* Puntero a la memoria compartida mapeada */
static HANDLE             g_hMapFile     = NULL; /* Handle del File Mapping */
static HANDLE             g_hSemEmpty    = NULL; /* Semafoto: slots libres en buffer circular */
static HANDLE             g_hSemFull     = NULL; /* Semafoto: slots ocupados en buffer circular */
static HANDLE             g_hMutex       = NULL; /* Mutex: acceso exclusivo al buffer */
static HANDLE             g_hShutdownEvt = NULL; /* Evento: senal de apagado ordenado (para M4) */
static HANDLE             g_hDebugEvt    = NULL; /* Evento: toggle de dump de depuracion (para M4) */
static PIPE_SLOT          g_slots[BROKER_MAX_PIPES]; /* Arreglo de slots de Named Pipes */
static DWORD              g_dwSlotCount  = 0;   /* Numero real de slots creados */
static volatile BOOL      g_bRunning     = TRUE;/* Bandera de ejecucion (volatile para hilos) */
static BOOL               g_bDebugDump   = FALSE;/* Estado del dump de depuracion */

/* ==================================================================
 *  FUNCIONES AUXILIARES
 * ================================================================== */

/* Convierte un tipo de sensor numerico a su representacion en cadena.
 * Los valores coinciden con la enumeracion SENSOR_TYPE de common.h */
static const char*
SensorTypeToStr(DWORD dwType)
{
    switch (dwType) {
    case SENSOR_TYPE_ENGINE: return "Engine";
    case SENSOR_TYPE_TIRES:  return "Tires";
    case SENSOR_TYPE_BRAKES: return "Brakes";
    case SENSOR_TYPE_GPS:    return "GPS";
    default:                 return "Unknown";
    }
}

/* Construye el nombre completo del Named Pipe para un slot dado.
 * El formato debe coincidir con el que usa el Modulo 1 (sensor.c):
 *   \\.\pipe\TCC_Sensor_<Type>_<Id>
 * Ejemplo: \\.\pipe\TCC_Sensor_Engine_1 */
static void
PipeNameForIndex(DWORD dwIndex, DWORD dwType, DWORD dwId, CHAR* pszOut, DWORD dwSize)
{
    (void)dwIndex;
    snprintf(pszOut, dwSize, "%s%s_%lu",
             TCC_PIPE_PREFIX, SensorTypeToStr(dwType), dwId);
}

/* ==================================================================
 *  DUMP DE DEPURACION (debug dump)
 *
 *  Funcion condicional: solo imprime cuando g_bDebugDump es TRUE.
 *  El Monitor (M4) activa/desactiva esto mediante el evento
 *  TCC_DebugEvent
 * ================================================================== */

static void
DebugPrint(const char* pszFmt, ...)
{
    if (!g_bDebugDump) return;

    va_list args;
    va_start(args, pszFmt);
    vprintf(pszFmt, args);
    va_end(args);
}

/* ==================================================================
 *  MANEJADOR DE CIERRE (Ctrl+C / Ctrl+Break / Cierre de consola)
 *
 *  Instalado via SetConsoleCtrlHandler en Broker_Iniciar.
 *  Al recibir la senal:
 *    1. Marca g_bRunning = FALSE para que todos los hilos sepan
 *       que deben terminar
 *    2. Senaliza el shutdown event (desbloquea WaitForMultipleObjects
 *       en los listener threads)
 *    3. Libera semaforos (desbloquea posibles lectores bloqueados)
 * ================================================================== */

static BOOL WINAPI
CtrlHandler(DWORD dwCtrlType)
{
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        printf("\n[ BROKER ] Ctrl+C received. Initiating orderly shutdown...\n");
        g_bRunning = FALSE;

        /* Desbloquear listener threads esperando en WaitForMultipleObjects */
        if (g_hShutdownEvt) {
            SetEvent(g_hShutdownEvt);
        }
        /* Desbloquear reader threads bloqueados en semaforos */
        if (g_hSemEmpty) {
            ReleaseSemaphore(g_hSemEmpty, BROKER_MAX_PIPES, NULL);
        }
        if (g_hSemFull) {
            ReleaseSemaphore(g_hSemFull, BROKER_MAX_PIPES, NULL);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

/* ==================================================================
 *  BUFFER CIRCULAR EN MEMORIA COMPARTIDA
 *
 *  Buffer alojado en un File Mapping de Windows (visible por M3 y M4).
 *  Protegido por:
 *    - Sem_Vacio: garantiza que haya espacio antes de escribir
 *    - Sem_Lleno: notifica al consumidor que hay datos disponibles
 *    - Mutex:     exclusion mutua en las operaciones de lectura/escritura
 *
 *  Mecanismo de backpressure:
 *    Cuando cnt == CAP_BUF, Sem_Vacio esta en 0, entonces
 *    Broker_BufferEscribir se bloquea. Esto detiene el flujo de
 *    lectura del pipe, y el pipe retiene datos, bloqueando al sensor.
 * ================================================================== */

/* Inicializa el buffer circular a cero y reinicia todos los contadores */
void
Broker_BufferInit(void)
{
    ZeroMemory(g_pShared, sizeof(SHARED_BUFFER));
    g_pShared->cabeza = 0;                     /* Indice de escritura */
    g_pShared->cola   = 0;                     /* Indice de lectura */
    g_pShared->cnt    = 0;                     /* Elementos actuales */
    g_pShared->dwEventsReceived      = 0;      /* Total de eventos recibidos */
    g_pShared->dwActiveSensors       = 0;      /* Sensores actualmente conectados */
    g_pShared->dwBufferMaxOccupancy  = 0;      /* Ocupacion maxima historica */
}

/* Escribe un evento en el buffer circular.
 *
 * Flujo:
 *   1. Espera a que haya un slot libre (Sem_Vacio)
 *   2. Adquiere el mutex para acceso exclusivo
 *   3. Escribe el evento en la posicion cabeza % CAP_BUF
 *   4. Incrementa cabeza y cnt
 *   5. Actualiza la marca de ocupacion maxima si es necesario
 *   6. Libera el mutex
 *   7. Libera Sem_Lleno para notificar al consumidor
 *
 * Retorna TRUE en exito, FALSE si falla (error en WaitForSingleObject) */
BOOL
Broker_BufferEscribir(const Evento* pEvento)
{
    /* BACKPRESSURE: si el buffer esta lleno, este Wait se bloquea
     * hasta que un lector (Dispatcher M3) libere un slot.
     * Mientras este hilo esta bloqueado, no lee del pipe,
     * entonces el pipe retiene datos, y el sensor se bloquea
     * en WriteFile. Esto es backpressure natural. */
    if (WaitForSingleObject(g_hSemEmpty, INFINITE) != WAIT_OBJECT_0)
        return FALSE;

    /* Exclusion mutua: solo un hilo escribe/lee a la vez */
    if (WaitForSingleObject(g_hMutex, INFINITE) != WAIT_OBJECT_0) {
        ReleaseSemaphore(g_hSemEmpty, 1, NULL);
        return FALSE;
    }

    /* Escritura circular en el buffer */
    g_pShared->items[g_pShared->cabeza % CAP_BUF] = *pEvento;
    g_pShared->cabeza++;
    g_pShared->cnt++;

    /* Registrar ocupacion maxima historica (para el Monitor M4) */
    if (g_pShared->cnt > g_pShared->dwBufferMaxOccupancy) {
        g_pShared->dwBufferMaxOccupancy = g_pShared->cnt;
    }

    ReleaseMutex(g_hMutex);
    ReleaseSemaphore(g_hSemFull, 1, NULL);      /* Notificar dato disponible */

    InterlockedIncrement(&g_pShared->dwEventsReceived);  /* Estadistica atomica */

    return TRUE;
}

/* ==================================================================
 *  HILO LECTOR POR SENSOR CONECTADO
 *
 *  Creado via CreateThread por el ListenerThread cuando un sensor
 *  se conecta al Named Pipe. Lee eventos del pipe y los deposita
 *  en el buffer circular compartido.
 *
 *  Ciclo de vida:
 *    1. Lee SENSOR_EVENT_HEADER (bloqueante en ReadFile)
 *    2. Lee el payload de longitud variable
 *    3. Convierte los datos crudos a un struct Evento
 *    4. Asigna prioridad segun el tipo de sensor
 *    5. Llama a Broker_BufferEscribir (puede bloquearse por backpressure)
 *    6. Repite hasta que el sensor se desconecte o haya shutdown
 *    7. Cierra el handle del pipe (solo en desconexion normal)
 *    8. Decrementa el contador de sensores activos
 * ================================================================== */

DWORD WINAPI
Broker_ReaderThread(LPVOID lpParam)
{
    PIPE_SLOT* pSlot = (PIPE_SLOT*)lpParam;
    HANDLE hPipe = pSlot->hPipe;                /* Handle del pipe (copia local) */
    DWORD dwSensorType = pSlot->dwSensorType;
    DWORD dwSensorId   = pSlot->dwSensorId;
    BYTE  headerBuf[sizeof(SENSOR_EVENT_HEADER)];  /* Buffer para el header empaquetado */
    BYTE  payloadBuf[TCC_MAX_PAYLOAD_SIZE];         /* Buffer para el payload */
    DWORD dwEventsRead = 0;

    DebugPrint("[ BROKER ] Reader started for %s #%lu\n",
               SensorTypeToStr(dwSensorType), dwSensorId);

    /* Bucle principal de lectura: se ejecuta mientras el sistema esta activo */
    while (g_bRunning) {
        DWORD dwRead = 0;

        /* PASO 1: Leer el header del evento (estructura empaquetada fija).
         * ReadFile es bloqueante: el hilo se duerme hasta que lleguen datos
         * o el pipe se desconecte. NO hay busy waiting. */
        if (!ReadFile(hPipe, headerBuf, sizeof(SENSOR_EVENT_HEADER), &dwRead, NULL)) {
            DWORD dwErr = GetLastError();
            /* ERROR_BROKEN_PIPE: el sensor se desconecto normalmente */
            if (dwErr == ERROR_BROKEN_PIPE || dwErr == ERROR_PIPE_NOT_CONNECTED) {
                DebugPrint("[ BROKER ] %s #%lu disconnected (events read: %lu).\n",
                           SensorTypeToStr(dwSensorType), dwSensorId, dwEventsRead);
            } else if (g_bRunning) {
                fprintf(stderr, "[ BROKER ] ReadFile error %lu on %s #%lu\n",
                        dwErr, SensorTypeToStr(dwSensorType), dwSensorId);
            }
            break;
        }

        /* Verificar que se leyo el header completo */
        if (dwRead != sizeof(SENSOR_EVENT_HEADER)) {
            if (dwRead == 0) break;  /* Pipe cerrado sin datos */
            fprintf(stderr, "[ BROKER ] Partial header read: %lu bytes on %s #%lu\n",
                    dwRead, SensorTypeToStr(dwSensorType), dwSensorId);
            break;
        }

        SENSOR_EVENT_HEADER* pHeader = (SENSOR_EVENT_HEADER*)headerBuf;

        /* PASO 2: Leer el payload de longitud variable */
        DWORD dwPayloadSize = pHeader->dwPayloadSize;
        if (dwPayloadSize > TCC_MAX_PAYLOAD_SIZE) {
            dwPayloadSize = TCC_MAX_PAYLOAD_SIZE;  /* Acotar por seguridad */
        }

        DWORD dwPayloadRead = 0;
        if (dwPayloadSize > 0) {
            if (!ReadFile(hPipe, payloadBuf, dwPayloadSize, &dwPayloadRead, NULL)) {
                DWORD dwErr = GetLastError();
                if (dwErr == ERROR_BROKEN_PIPE || dwErr == ERROR_PIPE_NOT_CONNECTED) {
                    DebugPrint("[ BROKER ] %s #%lu disconnected mid-payload.\n",
                               SensorTypeToStr(dwSensorType), dwSensorId);
                } else {
                    fprintf(stderr, "[ BROKER ] Payload ReadFile error %lu on %s #%lu\n",
                            dwErr, SensorTypeToStr(dwSensorType), dwSensorId);
                }
                break;
            }
        }

        /* PASO 3: Convertir los datos crudos a un struct Evento
         * para el consumo del Dispatcher (M3) */
        Evento evento;
        evento.idSensor = pHeader->dwSensorId;
        evento.idEvento = pHeader->dwEventId;
        evento.tipo     = (SENSOR_TYPE)pHeader->bySensorType;
        evento.ts       = pHeader->ullTimestamp;

        /* PASO 4: Asignar prioridad segun criticidad del sensor
         *   - ENGINE: CRIT (fallo de motor es critico)
         *   - BRAKES: ALTA (seguridad)
         *   - TIRES:  NORM (desgaste progresivo)
         *   - GPS:    BAJA (solo navegacion) */
        switch (evento.tipo) {
        case SENSOR_TYPE_ENGINE: evento.prio = CRIT; break;
        case SENSOR_TYPE_BRAKES: evento.prio = ALTA; break;
        case SENSOR_TYPE_TIRES:  evento.prio = NORM; break;
        case SENSOR_TYPE_GPS:    evento.prio = BAJA; break;
        default:                 evento.prio = NORM; break;
        }

        /* Copiar el payload al arreglo datos[4] del Evento
         * (los primeros 16 bytes del payload se interpretan como 4 enteros) */
        ZeroMemory(evento.datos, sizeof(evento.datos));
        DWORD dwCopyBytes = dwPayloadRead < sizeof(evento.datos)
                            ? dwPayloadRead : (DWORD)sizeof(evento.datos);
        memcpy(evento.datos, payloadBuf, dwCopyBytes);

        /* PASO 5: Escribir en el buffer circular compartido.
         * Esta llamada puede BLOQUEARSE si el buffer esta lleno
         * (backpressure). Mientras este hilo esta bloqueado, no
         * lee del pipe, y el sensor terminara bloqueandose tambien. */
        if (!Broker_BufferEscribir(&evento)) {
            fprintf(stderr, "[ BROKER ] Buffer write FAILED for %s #%lu event #%lu\n",
                    SensorTypeToStr(dwSensorType), dwSensorId, pHeader->dwEventId);
            break;
        }

        dwEventsRead++;

        /* Mostrar progreso si el debug dump esta activado */
        if (g_bDebugDump && (dwEventsRead % 50 == 0)) {
            printf("[ BROKER ] %s #%lu: %lu events ingested\n",
                   SensorTypeToStr(dwSensorType), dwSensorId, dwEventsRead);
        }

        /* Verificar si el Monitor (M4) solicito toggle de debug dump */
        if (g_hDebugEvt && WaitForSingleObject(g_hDebugEvt, 0) == WAIT_OBJECT_0) {
            g_bDebugDump = !g_bDebugDump;
            ResetEvent(g_hDebugEvt);
            printf("[ BROKER ] Debug dump %s\n",
                   g_bDebugDump ? "ENABLED" : "DISABLED");
        }
    }

    /* LIMPIEZA: el hilo termina */

    /* Decrementar contador de sensores activos en memoria compartida */
    InterlockedDecrement(&g_pShared->dwActiveSensors);

    /* Cerrar el pipe SOLO si fue una desconexion normal (g_bRunning == TRUE).
     * Si es un shutdown, Broker_Detener ya cerro el handle y no debemos
     * hacer doble close. */
    if (hPipe && hPipe != INVALID_HANDLE_VALUE) {
        if (g_bRunning) {
            FlushFileBuffers(hPipe);
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            pSlot->hPipe = INVALID_HANDLE_VALUE;
            DebugPrint("[ BROKER ] Pipe handle closed for %s #%lu\n",
                       SensorTypeToStr(dwSensorType), dwSensorId);
        }
    }

    DebugPrint("[ BROKER ] Reader thread exiting for %s #%lu\n",
               SensorTypeToStr(dwSensorType), dwSensorId);
    return 0;
}

/* ==================================================================
 *  HILO ESCUCHADOR POR SLOT DE PIPE
 *
 *  Cada slot tiene su propio listener thread que:
 *    1. Crea un Named Pipe con CreateNamedPipeA
 *    2. Espera conexion con ConnectNamedPipe (BLOQUEANTE, sin busy waiting)
 *    3. Cuando un sensor se conecta, crea un ReaderThread dedicado
 *    4. Espera a que el ReaderThread termine o se reciba senal de shutdown
 *    5. Si fue desconexion normal, crea una nueva instancia del pipe y
 *       vuelve al paso 1 (permitiendo reconexion)
 *    6. Si fue shutdown, termina el bucle
 * ================================================================== */

DWORD WINAPI
Broker_ListenerThread(LPVOID lpParam)
{
    PIPE_SLOT* pSlot = (PIPE_SLOT*)lpParam;

    DebugPrint("[ BROKER ] Listener started for %s #%lu on %s\n",
               SensorTypeToStr(pSlot->dwSensorType), pSlot->dwSensorId,
               pSlot->szPipeName);

    while (g_bRunning) {
        /* PASO 1: Crear una instancia del Named Pipe.
         * Usamos PIPE_UNLIMITED_INSTANCES para permitir que se creen
         * nuevas instancias del mismo nombre de pipe cuando un sensor
         * se reconecte. */
        pSlot->hPipe = CreateNamedPipeA(
            pSlot->szPipeName,
            PIPE_ACCESS_INBOUND,               /* Solo lectura (sensor escribe) */
            PIPE_TYPE_MESSAGE |                /* Flujo de mensajes */
            PIPE_READMODE_MESSAGE |            /* Lectura por mensajes */
            PIPE_WAIT,                         /* Modo bloqueante */
            PIPE_UNLIMITED_INSTANCES,          /* Multiples instancias */
            0,                                 /* Tamano de salida (0 = default) */
            TCC_PIPE_BUFFER_SIZE,              /* Tamano del buffer de entrada */
            TCC_PIPE_TIMEOUT,                  /* Timeout por defecto */
            NULL                               /* Atributos de seguridad por defecto */
        );

        if (pSlot->hPipe == INVALID_HANDLE_VALUE) {
            DWORD dwErr = GetLastError();
            if (g_bRunning) {
                fprintf(stderr, "[ BROKER ] CreateNamedPipe failed for %s: error %lu\n",
                        pSlot->szPipeName, dwErr);
            }
            Sleep(1000);  /* Esperar antes de reintentar */
            continue;
        }

        DebugPrint("[ BROKER ] Waiting for connection on %s...\n", pSlot->szPipeName);

        /* PASO 2: Esperar conexion de un sensor.
         * ConnectNamedPipe es BLOQUEANTE: el hilo se duerme hasta que
         * un sensor se conecte via CreateFile. NO hay busy waiting.
         * El consumo de CPU durante esta espera es 0%. */
        BOOL bConnected = ConnectNamedPipe(pSlot->hPipe, NULL);
        if (!bConnected) {
            DWORD dwErr = GetLastError();
            /* ERROR_OPERATION_ABORTED: el pipe fue cerrado durante shutdown */
            if (dwErr == ERROR_OPERATION_ABORTED || !g_bRunning) {
                CloseHandle(pSlot->hPipe);
                pSlot->hPipe = INVALID_HANDLE_VALUE;
                break;
            }
            /* ERROR_PIPE_CONNECTED: el cliente ya se conecto antes de
             * que llamaramos a ConnectNamedPipe (condicion de carrera
             * normal en Named Pipes) */
            if (dwErr != ERROR_PIPE_CONNECTED) {
                if (g_bRunning) {
                    fprintf(stderr, "[ BROKER ] ConnectNamedPipe failed for %s: error %lu\n",
                            pSlot->szPipeName, dwErr);
                }
                CloseHandle(pSlot->hPipe);
                pSlot->hPipe = INVALID_HANDLE_VALUE;
                continue;
            }
            bConnected = TRUE;
        }

        /* Verificar shutdown justo despues de la conexion */
        if (!g_bRunning) {
            DisconnectNamedPipe(pSlot->hPipe);
            CloseHandle(pSlot->hPipe);
            pSlot->hPipe = INVALID_HANDLE_VALUE;
            break;
        }

        printf("[ BROKER ] Sensor %s #%lu CONNECTED\n",
               SensorTypeToStr(pSlot->dwSensorType), pSlot->dwSensorId);

        pSlot->bConnected = TRUE;
        InterlockedIncrement(&g_pShared->dwActiveSensors);

        /* Registrar los datos del sensor en la tabla compartida */
        DWORD dwSlotIdx = (DWORD)(pSlot - g_slots);
        if (dwSlotIdx < g_dwSlotCount) {
            g_pShared->adwSensorTypes[dwSlotIdx] = pSlot->dwSensorType;
            g_pShared->adwSensorIds[dwSlotIdx]   = pSlot->dwSensorId;
        }

        /* PASO 3: Crear hilo lector dedicado para este sensor */
        pSlot->hReaderThread = CreateThread(NULL, 0, Broker_ReaderThread, pSlot, 0, NULL);
        if (!pSlot->hReaderThread) {
            fprintf(stderr, "[ BROKER ] Failed to create reader thread for %s #%lu\n",
                    SensorTypeToStr(pSlot->dwSensorType), pSlot->dwSensorId);
            FlushFileBuffers(pSlot->hPipe);
            DisconnectNamedPipe(pSlot->hPipe);
            CloseHandle(pSlot->hPipe);
            pSlot->hPipe = INVALID_HANDLE_VALUE;
            pSlot->bConnected = FALSE;
            InterlockedDecrement(&g_pShared->dwActiveSensors);
            continue;
        }

        /* PASO 4: Esperar a que el reader termine O que llegue shutdown.
         * WaitForMultipleObjects con INFINITE bloquea pasivamente. */
        HANDLE hWaitHandles[2] = { pSlot->hReaderThread, g_hShutdownEvt };
        DWORD dwWait = WaitForMultipleObjects(2, hWaitHandles, FALSE, INFINITE);

        /* Cerrar handle del thread (el thread ya termino) */
        CloseHandle(pSlot->hReaderThread);
        pSlot->hReaderThread = NULL;

        /* Si se recibio shutdown, salir del bucle */
        if (dwWait == WAIT_OBJECT_0 + 1) {
            DebugPrint("[ BROKER ] Shutdown event received in listener for %s #%lu\n",
                       SensorTypeToStr(pSlot->dwSensorType), pSlot->dwSensorId);
            break;
        }

        /* PASO 5: El reader termino (sensor se desconecto).
         * El reader ya cerro el handle del pipe (si fue desconexion normal).
         * Volvemos al inicio del bucle para crear una nueva instancia
         * y esperar la proxima conexion. */
        DebugPrint("[ BROKER ] Listener loop continuing for %s #%lu\n",
                   SensorTypeToStr(pSlot->dwSensorType), pSlot->dwSensorId);
    }

    DebugPrint("[ BROKER ] Listener thread exiting for %s #%lu\n",
               SensorTypeToStr(pSlot->dwSensorType), pSlot->dwSensorId);
    return 0;
}

/* ==================================================================
 *  CREACION DE PIPES CON NOMBRE
 *
 *  Configura los slots de Named Pipes que el Broker va a escuchar.
 *  Por defecto crea 2 instancias por cada tipo de sensor:
 *    - Engine: IDs 1, 2
 *    - Tires:  IDs 1, 2
 *    - Brakes: IDs 1, 2
 *    - GPS:    IDs 1, 2
 *
 *  Cada slot crea un ListenerThread que manejara las conexiones
 *  entrantes para ese pipe en especifico.
 * ================================================================== */

BOOL
Broker_CrearPipes(void)
{
    /* Matriz de configuracion: tipo de sensor y sus IDs */
    static const DWORD tipoPorSlot[] = {
        SENSOR_TYPE_ENGINE, SENSOR_TYPE_ENGINE,
        SENSOR_TYPE_TIRES,  SENSOR_TYPE_TIRES,
        SENSOR_TYPE_BRAKES, SENSOR_TYPE_BRAKES,
        SENSOR_TYPE_GPS,    SENSOR_TYPE_GPS
    };
    static const DWORD idPorSlot[] = {
        1, 2,
        1, 2,
        1, 2,
        1, 2
    };

    DWORD dwCount = sizeof(tipoPorSlot) / sizeof(tipoPorSlot[0]);
    if (dwCount > BROKER_MAX_PIPES) {
        dwCount = BROKER_MAX_PIPES;
    }

    g_dwSlotCount = dwCount;
    ZeroMemory(g_slots, sizeof(g_slots));

    for (DWORD i = 0; i < dwCount; i++) {
        PIPE_SLOT* pSlot = &g_slots[i];
        pSlot->dwSensorType = tipoPorSlot[i];
        pSlot->dwSensorId   = idPorSlot[i];
        pSlot->hPipe        = INVALID_HANDLE_VALUE;
        pSlot->bConnected   = FALSE;
        pSlot->bActive      = TRUE;

        /* Construir nombre del pipe: \\.\pipe\TCC_Sensor_<Type>_<Id> */
        PipeNameForIndex(i, pSlot->dwSensorType, pSlot->dwSensorId,
                         pSlot->szPipeName, sizeof(pSlot->szPipeName));

        /* Crear hilo escuchador para este slot.
         * Cada listener maneja EXACTAMENTE UN nombre de pipe, y acepta
         * conexiones en un bucle infinito hasta el shutdown. */
        pSlot->hListenerThread = CreateThread(NULL, 0, Broker_ListenerThread, pSlot, 0, NULL);
        if (!pSlot->hListenerThread) {
            fprintf(stderr, "[ BROKER ] Failed to create listener thread for slot %lu\n", i);
            /* Rollback: cerrar threads creados hasta ahora */
            for (DWORD j = 0; j < i; j++) {
                if (g_slots[j].hListenerThread) {
                    CloseHandle(g_slots[j].hListenerThread);
                    g_slots[j].hListenerThread = NULL;
                }
            }
            return FALSE;
        }

        printf("[ BROKER ] Pipe slot %lu: %s\n", i, pSlot->szPipeName);
    }

    return TRUE;
}

/* ==================================================================
 *  INICIALIZACION PRINCIPAL DEL BROKER
 *
 *  Orden de inicializacion:
 *    1. Instalar manejador de Ctrl+C
 *    2. Crear File Mapping (memoria compartida para M3 y M4)
 *    3. Crear semaforos named (Vacio y Lleno) para sincronizacion IPC
 *    4. Crear mutex named para exclusion mutua
 *    5. Crear eventos named (shutdown y debug) para coordinacion con M4
 *    6. Crear Named Pipes y sus listener threads
 *
 *  Si cualquer paso falla, llama a Broker_Detener() para hacer rollup
 *  y liberar los recursos ya adquiridos.
 * ================================================================== */

BOOL
Broker_Iniciar(void)
{
    printf("============================================\n");
    printf("  TCC-System - Broker Ingestor Central\n");
    printf("  Modulo 2 - Proceso Multihilo\n");
    printf("============================================\n\n");

    /* --- 1. Instalar manejador de senales --- */
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
        fprintf(stderr, "[ BROKER ] SetConsoleCtrlHandler failed\n");
        return FALSE;
    }

    /* --- 2. Crear Memoria Compartida (File Mapping) --- */
    /* Esta memoria es visible por otros procesos a traves del nombre
     * "TCC_SharedBuffer". El Dispatcher (M3) y el Monitor (M4) pueden
     * abrirla con OpenFileMappingA. */
    g_hMapFile = CreateFileMappingA(
        INVALID_HANDLE_VALUE,     /* Usar memoria del sistema (pagina de swap) */
        NULL,                     /* Atributos de seguridad por defecto */
        PAGE_READWRITE,           /* Lectura/escritura */
        0,                        /* Tamano alto (32 bits -> 0) */
        (DWORD)sizeof(SHARED_BUFFER), /* Tamano bajo */
        TCC_SHARED_MEM_NAME       /* Nombre unico para IPC */
    );

    if (!g_hMapFile) {
        fprintf(stderr, "[ BROKER ] CreateFileMapping failed: error %lu\n", GetLastError());
        return FALSE;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "[ BROKER ] Shared memory already exists (another broker running?)\n");
        CloseHandle(g_hMapFile);
        return FALSE;
    }

    /* Mapear la memoria compartida en el espacio de direcciones de este proceso */
    g_pShared = (SHARED_BUFFER*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g_pShared) {
        fprintf(stderr, "[ BROKER ] MapViewOfFile failed: error %lu\n", GetLastError());
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return FALSE;
    }

    Broker_BufferInit();
    printf("[ BROKER ] Shared memory created (%zu bytes)\n", sizeof(SHARED_BUFFER));

    /* --- 3. Crear Semaforos con nombre (IPC) --- */
    /* Named Semaphores permiten que el Dispatcher (M3) abra los mismos
     * semaforos para coordinar el acceso al buffer circular. */
    g_hSemEmpty = CreateSemaphoreA(NULL, CAP_BUF, CAP_BUF, TCC_SEM_EMPTY_NAME);
    if (!g_hSemEmpty) {
        fprintf(stderr, "[ BROKER ] CreateSemaphore (empty) failed: error %lu\n", GetLastError());
        Broker_Detener();
        return FALSE;
    }

    g_hSemFull = CreateSemaphoreA(NULL, 0, CAP_BUF, TCC_SEM_FULL_NAME);
    if (!g_hSemFull) {
        fprintf(stderr, "[ BROKER ] CreateSemaphore (full) failed: error %lu\n", GetLastError());
        Broker_Detener();
        return FALSE;
    }

    /* --- 4. Crear Mutex con nombre (IPC) --- */
    g_hMutex = CreateMutexA(NULL, FALSE, TCC_MUTEX_NAME);
    if (!g_hMutex) {
        fprintf(stderr, "[ BROKER ] CreateMutex failed: error %lu\n", GetLastError());
        Broker_Detener();
        return FALSE;
    }

    printf("[ BROKER ] Synchronization objects created (semaphores + mutex)\n");

    /* --- 5. Crear Eventos de Coordinacion (para Modulo 4 - Monitor) --- */
    /* El Monitor puede SetEvent en TCC_ShutdownEvent para ordenar un
     * apagado controlado, o en TCC_DebugEvent para activar/desactivar
     * el dump de depuracion. */
    g_hShutdownEvt = CreateEventA(NULL, TRUE, FALSE, TCC_EVT_SHUTDOWN_NAME);
    g_hDebugEvt    = CreateEventA(NULL, TRUE, FALSE, TCC_EVT_DEBUG_NAME);

    if (!g_hShutdownEvt || !g_hDebugEvt) {
        fprintf(stderr, "[ BROKER ] CreateEvent failed: error %lu\n", GetLastError());
        Broker_Detener();
        return FALSE;
    }

    printf("[ BROKER ] Coordination events created (shutdown + debug)\n");

    /* --- 6. Crear Pipes con Nombre e Hilos Escuchadores --- */
    if (!Broker_CrearPipes()) {
        Broker_Detener();
        return FALSE;
    }

    printf("\n[ BROKER ] System ready. Waiting for sensor connections...\n");
    printf("[ BROKER ] Press Ctrl+C to stop.\n\n");
    return TRUE;
}

/* ==================================================================
 *  DETENCION ORDENADA Y LIMPIEZA DE RECURSOS
 *
 *  Broker_Detener debe llamarse para hacer un shutdown limpio.
 *  Secuencia:
 *    1. Marcar g_bRunning = FALSE (aviso a todos los hilos)
 *    2. Senalizar shutdown event y liberar semaforos
 *    3. Cerrar todos los handles de pipe activos (desbloquea I/O pendiente)
 *    4. Esperar a que los listener/reader threads terminen (con timeout)
 *    5. Cerrar handles de threads
 *    6. Cerrar semaforos, mutex, eventos
 *    7. Desmapear y cerrar la memoria compartida
 *    8. Desinstalar el manejador de Ctrl+C
 *
 *  Ningun recurso del sistema operativo queda abierto.
 * ================================================================== */

/* Limpia un slot individual: espera a que sus threads terminen
 * y cierra todos los handles asociados. */
void
Broker_LimpiarSlot(PIPE_SLOT* pSlot)
{
    DebugPrint("[ BROKER ] Cleaning up slot %s #%lu\n",
               SensorTypeToStr(pSlot->dwSensorType), pSlot->dwSensorId);

    if (pSlot->hReaderThread) {
        WaitForSingleObject(pSlot->hReaderThread, 5000);  /* Esperar max 5s */
        CloseHandle(pSlot->hReaderThread);
        pSlot->hReaderThread = NULL;
    }

    if (pSlot->hListenerThread) {
        WaitForSingleObject(pSlot->hListenerThread, 5000); /* Esperar max 5s */
        CloseHandle(pSlot->hListenerThread);
        pSlot->hListenerThread = NULL;
    }

    if (pSlot->hPipe && pSlot->hPipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(pSlot->hPipe);
        DisconnectNamedPipe(pSlot->hPipe);
        CloseHandle(pSlot->hPipe);
        pSlot->hPipe = INVALID_HANDLE_VALUE;
    }
}

/* Detiene todo el sistema y libera todos los recursos */
void
Broker_Detener(void)
{
    printf("\n[ BROKER ] Shutting down...\n");

    g_bRunning = FALSE;

    /* Senalizar eventos de parada */
    if (g_hShutdownEvt) {
        SetEvent(g_hShutdownEvt);
    }
    if (g_hSemEmpty) {
        ReleaseSemaphore(g_hSemEmpty, BROKER_MAX_PIPES, NULL);
    }
    if (g_hSemFull) {
        ReleaseSemaphore(g_hSemFull, BROKER_MAX_PIPES, NULL);
    }

    /* Cerrar todos los handles de pipe activos para desbloquear
     * ConnectNamedPipe y ReadFile en los hilos escuchadores/lectores.
     * CancelIoEx asegura que cualquier I/O pendiente se cancele. */
    for (DWORD i = 0; i < g_dwSlotCount; i++) {
        PIPE_SLOT* pSlot = &g_slots[i];
        if (pSlot->hPipe && pSlot->hPipe != INVALID_HANDLE_VALUE) {
            CancelIoEx(pSlot->hPipe, NULL);
            CloseHandle(pSlot->hPipe);
            pSlot->hPipe = INVALID_HANDLE_VALUE;
        }
    }

    /* Esperar y cerrar hilos escuchadores y lectores */
    for (DWORD i = 0; i < g_dwSlotCount; i++) {
        Broker_LimpiarSlot(&g_slots[i]);
    }

    /* Liberar objetos de sincronizacion */
    if (g_hSemEmpty) { CloseHandle(g_hSemEmpty); g_hSemEmpty = NULL; }
    if (g_hSemFull)  { CloseHandle(g_hSemFull);  g_hSemFull  = NULL; }
    if (g_hMutex)    { CloseHandle(g_hMutex);    g_hMutex    = NULL; }
    if (g_hShutdownEvt) { CloseHandle(g_hShutdownEvt); g_hShutdownEvt = NULL; }
    if (g_hDebugEvt)    { CloseHandle(g_hDebugEvt);    g_hDebugEvt    = NULL; }

    /* Liberar memoria compartida */
    if (g_pShared) {
        UnmapViewOfFile(g_pShared);    /* Desmapear la vista */
        g_pShared = NULL;
    }
    if (g_hMapFile) {
        CloseHandle(g_hMapFile);       /* Cerrar el File Mapping */
        g_hMapFile = NULL;
    }

    SetConsoleCtrlHandler(CtrlHandler, FALSE);

    printf("[ BROKER ] All handles closed. Shutdown complete.\n");
}

/* ==================================================================
 *  PUNTO DE ENTRADA
 *
 *  Inicializa el Broker, ejecuta el bucle principal de monitoreo
 *  y realiza el shutdown ordenado al terminar.
 *
 *  Bucle principal:
 *    - Espera el shutdown event (con timeout de 1s para poder
 *      verificar g_bRunning periodicamente)
 *    - Verifica el debug event (toggle de dump de depuracion)
 *    - Muestra estadisticas en tiempo real si debug dump activo
 * ================================================================== */

int
main(void)
{
    /* Deshabilitar buffering de stdout/stderr para salida en tiempo real */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    if (!Broker_Iniciar()) {
        Broker_Detener();
        fprintf(stderr, "[ BROKER ] Initialization failed. Exiting.\n");
        return EXIT_FAILURE;
    }

    /* Bucle principal de monitoreo */
    while (g_bRunning) {
        /* Esperar shutdown event con timeout de 1 segundo.
         * Esto permite verificar g_bRunning periodicamente sin
         * bloqueo perpetuo (no es busy waiting porque WaitForSingleObject
         * es una espera pasiva del kernel). */
        DWORD dwWait = WaitForSingleObject(g_hShutdownEvt, 1000);
        if (dwWait == WAIT_OBJECT_0) {
            printf("[ BROKER ] Shutdown event signaled.\n");
            break;
        }

        if (!g_bRunning) break;

        /* Verificar si el Monitor (M4) solicito toggle del debug dump */
        if (g_hDebugEvt && WaitForSingleObject(g_hDebugEvt, 0) == WAIT_OBJECT_0) {
            g_bDebugDump = !g_bDebugDump;
            ResetEvent(g_hDebugEvt);
            printf("[ BROKER ] Debug dump %s\n",
                   g_bDebugDump ? "ENABLED" : "DISABLED");
        }

        /* Leer estadisticas de la memoria compartida */
        DWORD dwActive = g_pShared->dwActiveSensors;
        DWORD dwTotal  = g_pShared->dwEventsReceived;
        DWORD dwOcc    = g_pShared->cnt;

        if (g_bDebugDump) {
            printf("[ BROKER ] Stats - Active: %lu, Events: %lu, Buffer: %lu/%d\n",
                   dwActive, dwTotal, dwOcc, CAP_BUF);
        }

        /* Pequena pausa si hay sensores conectados pero cero eventos */
        if (dwActive > 0 && dwTotal == 0) {
            Sleep(100);
        }
    }

    Broker_Detener();
    printf("[ BROKER ] Broker terminated.\n");
    return EXIT_SUCCESS;
}
