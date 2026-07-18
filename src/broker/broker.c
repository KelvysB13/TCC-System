/*
 * broker.c - Módulo 2: Broker Ingestor Central
 *
 * ARQUITECTURA:
 * ┌─────────────────────────────────────────────────────────┐
 * │                    BROKER PROCESS                       │
 * │                                                         │
 * │  Main Thread ──▶ CreateNamedPipe ──▶ ConnectNamedPipe   │
 * │       │                        (overlapped + evento)    │
 * │       │                        │                        │
 * │       ▼                        ▼                        │
 * │  ┌──────────┐  ┌──────────┐  ┌──────────┐              │
 * │  │Receiver#1│  │Receiver#2│  │Receiver#N│  ...          │
 * │  │ (Thread) │  │ (Thread) │  │ (Thread) │              │
 * │  └────┬─────┘  └────┬─────┘  └────┬─────┘              │
 * │       │             │             │                     │
 * │       ▼             ▼             ▼                     │
 * │  ┌──────────────────────────────────────────────────┐   │
 * │  │         MEMORIA COMPARTIDA (File Mapping)        │   │
 * │  │  ┌─────────────────────────────────────────────┐ │   │
 * │  │  │         SharedCircularBuffer                │ │   │
 * │  │  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐       │ │   │
 * │  │  │  │ slot │ │ slot │ │ slot │ │ slot │  ...   │ │   │
 * │  │  │  │   0  │ │   1  │ │   2  │ │   3  │       │ │   │
 * │  │  │  └──────┘ └──────┘ └──────┘ └──────┘       │ │   │
 * │  │  │  ▲ writeIdx          ▲ readIdx              │ │   │
 * │  │  └─────────────────────────────────────────────┘ │   │
 * │  └──────────────────────────────────────────────────┘   │
 * │                                                         │
 * │  SINCRONIZACIÓN:                                        │
 * │  • g_hSemEmpty  → cuenta slots vacíos (prod-consumer)   │
 * │  • g_hSemFull   → cuenta slots llenos                   │
 * │  • g_hBufferMutex → acceso exclusivo al buffer          │
 * │  • g_hShutdownEvent → apagado coordinado                 │
 * └─────────────────────────────────────────────────────────┘
 *
 * FLUJO DE UN EVENTO:
 *   1. Sensor escribe TelemetryEvent en el Named Pipe
 *   2. Hilo receptor lee con ReadFile overlapped
 *   3. Espera slot vacío (WaitForMultipleObjects en semEmpty)
 *   4. Adquiere mutex, escribe en buffer[idx], libera mutex
 *   5. Libera semFull (avisa al Dispatcher que hay datos)
 *   6. El Dispatcher (Módulo 3) consume el evento
 */

#include "broker.h"
#include <stdio.h>

/* ===================================================================
 * VARIABLES GLOBALES (estáticas, solo visibles en este .c)
 * =================================================================== */

/* Handle del File Mapping de la memoria compartida */
static HANDLE g_hSharedMemory = NULL;

/* Handle del Mutex nombrado para acceso exclusivo al buffer circular */
static HANDLE g_hBufferMutex = NULL;

/*
 * Semáforo de slots vacíos.
 * Valor inicial = CIRCULAR_BUFFER_CAP (todos los slots están vacíos).
 * Los hilos productores (receiver threads) esperan aquí cuando
 * el buffer está lleno → BACKPRESSURE NATURAL.
 */
static HANDLE g_hSemEmpty = NULL;

/*
 * Semáforo de slots llenos.
 * Valor inicial = 0 (no hay datos aún).
 * Los hilos consumidores (Dispatcher) esperan aquí.
 */
static HANDLE g_hSemFull = NULL;

/*
 * Evento de señalización para shutdown.
 * Se usa en WaitForMultipleObjects junto con otros objetos
 * para poder despertar hilos bloqueados durante el apagado.
 */
static HANDLE g_hShutdownEvent = NULL;

/* Puntero a la memoria compartida mapeada */
static SharedCircularBuffer *g_pBuffer = NULL;

/* Contador atómico global para asignar IDs únicos a cada evento */
static volatile LONG g_nextEventId = 0;

/* Bandera de shutdown. volatile porque se modifica desde el
   manejador de consola (otro contexto de ejecución). */
static volatile BOOL g_shutdown = FALSE;


/* ===================================================================
 * Broker_ConsoleHandler - Manejador de señales de consola
 * ===================================================================
 *
 * Se registra con SetConsoleCtrlHandler. Windows lo invoca cuando
 * el usuario presiona Ctrl+C, Ctrl+Break o cierra la ventana.
 *
 * Su trabajo es:
 *   1. Poner g_shutdown en TRUE para que los bucles terminen
 *   2. Señalizar g_hShutdownEvent para despertar hilos bloqueados
 *      en WaitForMultipleObjects (los que incluyen este evento
 *      en su lista de espera)
 *
 * Retorna TRUE para indicar que manejamos la señal nosotros.
 */
BOOL WINAPI Broker_ConsoleHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT ||
        dwCtrlType == CTRL_BREAK_EVENT ||
        dwCtrlType == CTRL_CLOSE_EVENT) {
        g_shutdown = TRUE;
        if (g_hShutdownEvent) SetEvent(g_hShutdownEvent);
        return TRUE;
    }
    return FALSE;
}


/* ===================================================================
 * Broker_Init - Inicialización del Broker
 * ===================================================================
 *
 * Crea o abre todos los objetos del kernel necesarios:
 *
 *   1. Evento de shutdown (CreateEventA)
 *      - Crea el evento que coordinará el apagado entre procesos
 *      - Inicialmente no señalizado
 *
 *   2. File Mapping / Memoria Compartida (CreateFileMappingA)
 *      - Crea un área de memoria compartida entre procesos
 *      - Todos los módulos (Broker, Dispatcher, Monitor) pueden
 *        mapearla con MapViewOfFile
 *      - Si ya existe (ERROR_ALREADY_EXISTS), no se reinicializa
 *        desde cero a menos que sea necesario
 *
 *   3. Vista del File Mapping (MapViewOfFile)
 *      - Obtiene un puntero a la SharedCircularBuffer en el
 *        espacio de direcciones de este proceso
 *
 *   4. Mutex (CreateMutexA)
 *      - Protege el acceso concurrente al buffer circular
 *      - Es nombrado para que otros procesos puedan usarlo
 *
 *   5. Semáforo de vacíos (CreateSemaphoreA)
 *      - Inicia con CIRCULAR_BUFFER_CAP señales (todos vacíos)
 *      - Cada "wait" consume una señal (slot que se llena)
 *      - Cada "release" agrega una señal (slot que se vacía)
 *
 *   6. Semáforo de llenos (CreateSemaphoreA)
 *      - Inicia con 0 señales (ningún dato)
 *      - Cada "wait" consume una señal (slot que se lee)
 *      - Cada "release" agrega una señal (slot que se escribe)
 *
 *   7. Inicialización del buffer bajo el mutex:
 *      - Si es primera vez: ZeroMemory completo
 *      - Si es reinicio: resetea índices y reajusta semáforos
 *
 *   8. Reset del evento de shutdown (por si quedó señalizado
 *      de una ejecución anterior)
 */
BOOL Broker_Init(void)
{
    DWORD shmSize = sizeof(SharedCircularBuffer);
    BOOL  isNew   = FALSE;

    /* --- 1. Evento de shutdown --- */
    g_hShutdownEvent = CreateEventA(NULL, TRUE, FALSE, SHUTDOWN_EVENT_NAME);
    if (!g_hShutdownEvent) {
        printf("CreateEvent failed: %lu\n", GetLastError());
        return FALSE;
    }

    /* --- 2. File Mapping en memoria compartida --- */
    g_hSharedMemory = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                         PAGE_READWRITE, 0, shmSize, SHM_NAME);
    if (!g_hSharedMemory) {
        printf("CreateFileMapping failed: %lu\n", GetLastError());
        Broker_Shutdown();
        return FALSE;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) isNew = TRUE;

    /* --- 3. Mapear la vista del archivo a nuestro espacio de direcciones --- */
    g_pBuffer = (SharedCircularBuffer*)MapViewOfFile(g_hSharedMemory,
                                                     FILE_MAP_ALL_ACCESS, 0, 0, shmSize);
    if (!g_pBuffer) {
        printf("MapViewOfFile failed: %lu\n", GetLastError());
        Broker_Shutdown();
        return FALSE;
    }

    /* --- 4. Mutex para acceso exclusivo al buffer --- */
    g_hBufferMutex = CreateMutexA(NULL, FALSE, MUTEX_NAME);
    if (!g_hBufferMutex) {
        printf("CreateMutex failed: %lu\n", GetLastError());
        Broker_Shutdown();
        return FALSE;
    }

    /* --- 5. Semáforo de slots vacíos --- */
    g_hSemEmpty = CreateSemaphoreA(NULL, CIRCULAR_BUFFER_CAP,
                                    CIRCULAR_BUFFER_CAP, SEM_EMPTY_NAME);
    if (!g_hSemEmpty) {
        printf("CreateSemaphore (empty) failed: %lu\n", GetLastError());
        Broker_Shutdown();
        return FALSE;
    }

    /* --- 6. Semáforo de slots llenos --- */
    g_hSemFull = CreateSemaphoreA(NULL, 0, CIRCULAR_BUFFER_CAP, SEM_FULL_NAME);
    if (!g_hSemFull) {
        printf("CreateSemaphore (full) failed: %lu\n", GetLastError());
        Broker_Shutdown();
        return FALSE;
    }

    /* --- 7. Inicialización del buffer circular --- */
    /*
     * Adquirimos el mutex para garantizar exclusión mutua durante
     * la inicialización. Esto evita que el Dispatcher (Módulo 3)
     * intente leer del buffer mientras lo estamos reinicializando.
     *
     * isNew == TRUE:  La memoria compartida fue creada ahora.
     *                  Hacemos ZeroMemory completo de toda la estructura.
     *
     * isNew == FALSE: La memoria ya existía (reinicio del broker).
     *   - Si el flag initialized está en FALSE: primera vez real,
     *     hacemos ZeroMemory completo.
     *   - Si ya estaba inicializado: solo reseteamos índices y
     *     reajustamos los semáforos al estado "buffer vacío".
     */
    WaitForSingleObject(g_hBufferMutex, INFINITE);
    if (isNew || !g_pBuffer->initialized) {
        /* Primera creación: inicializar todo a cero */
        ZeroMemory(g_pBuffer, shmSize);
        g_pBuffer->bufferCapacity = CIRCULAR_BUFFER_CAP;
        g_pBuffer->bufferReadIndex = 0;
        g_pBuffer->bufferWriteIndex = 0;
        g_pBuffer->bufferCount = 0;
        g_pBuffer->eventsProcessed = 0;
        g_pBuffer->activeSensors = 0;
        g_pBuffer->initialized = TRUE;
    } else {
        /* Reinicio: resetear índices y estadísticas */
        g_pBuffer->bufferWriteIndex = 0;
        g_pBuffer->bufferReadIndex = 0;
        g_pBuffer->bufferCount = 0;
        g_pBuffer->eventsProcessed = 0;
        g_pBuffer->activeSensors = 0;

        /*
         * Reajustar semáforos al estado "buffer vacío":
         *   - Drenar semFull: consumir todas las señales pendientes
         *     (WaitForSingleObject con timeout=0, no hay busy-waiting
         *      porque retorna inmediatamente si no hay señal)
         *   - Restaurar semEmpty: liberar hasta CIRCULAR_BUFFER_CAP
         *     señales (nos detenemos si ya llegó al máximo)
         */
        while (WaitForSingleObject(g_hSemFull, 0) == WAIT_OBJECT_0);
        for (int i = 0; i < CIRCULAR_BUFFER_CAP; i++) {
            LONG prev;
            if (!ReleaseSemaphore(g_hSemEmpty, 1, &prev)) break;
            if (prev >= (LONG)(CIRCULAR_BUFFER_CAP - 1)) break;
        }
    }
    ReleaseMutex(g_hBufferMutex);

    /* --- 8. Resetear el evento de shutdown (por si acaso) --- */
    ResetEvent(g_hShutdownEvent);

    return TRUE;
}


/* ===================================================================
 * Broker_Shutdown - Liberación ordenada de todos los recursos
 * ===================================================================
 *
 * Esta función se llama durante el apagado del broker. Es OBLIGATORIO
 * que cada recurso del sistema operativo se cierre explícitamente:
 *
 *   • UnmapViewOfFile  → desmapea la vista de memoria compartida
 *   • CloseHandle       → cierra cada handle del kernel
 *
 * No se toleran fugas de handles (lo exige la consigna).
 * Cada CloseHandle es NULL-safe: si el handle es NULL, no hace nada.
 *
 * El orden de cierre es importante:
 *   1. Primero desmapear la vista (g_pBuffer)
 *   2. Luego cerrar el File Mapping
 *   3. Finalmente cerrar los objetos de sincronización
 */
void Broker_Shutdown(void)
{
    g_shutdown = TRUE;
    if (g_hShutdownEvent) SetEvent(g_hShutdownEvent);

    /* Desmapear la memoria compartida antes de cerrar el mapping */
    if (g_pBuffer) {
        UnmapViewOfFile(g_pBuffer);
        g_pBuffer = NULL;
    }

    /* Cerrar todos los handles del kernel */
    if (g_hSharedMemory)  { CloseHandle(g_hSharedMemory);  g_hSharedMemory  = NULL; }
    if (g_hBufferMutex)   { CloseHandle(g_hBufferMutex);   g_hBufferMutex   = NULL; }
    if (g_hSemEmpty)      { CloseHandle(g_hSemEmpty);      g_hSemEmpty      = NULL; }
    if (g_hSemFull)       { CloseHandle(g_hSemFull);       g_hSemFull       = NULL; }
    if (g_hShutdownEvent) { CloseHandle(g_hShutdownEvent); g_hShutdownEvent = NULL; }
}


/* ===================================================================
 * Broker_ReceiverThread - Hilo receptor de un sensor
 * ===================================================================
 *
 * Cada sensor conectado tiene su propio hilo. Este hilo:
 *
 *   1. Incrementa el contador atómico de sensores activos
 *   2. Crea un evento para la E/S overlapped
 *   3. Bucle principal (mientras no haya shutdown ni desconexión):
 *
 *      a. LEE del Named Pipe con ReadFile overlapped
 *         - Usamos FILE_FLAG_OVERLAPPED + OVERLAPPED para poder
 *           esperar simultáneamente entre la lectura y el shutdown
 *         - ReadFile lanza la operación y retorna inmediatamente
 *           (ERROR_IO_PENDING) o completa síncronamente (TRUE)
 *         - En ambos casos, esperamos en WaitForMultipleObjects
 *           entre el evento de lectura y el evento de shutdown
 *
 *      b. ASIGNA ID único y timestamp de alta resolución
 *         - eventId: contador atómico global (InterlockedIncrement)
 *         - timestamp: QueryPerformanceCounter (alta精确度)
 *
 *      c. ESPERA slot vacío en el buffer circular
 *         - WaitForMultipleObjects(g_hSemEmpty, g_hShutdownEvent)
 *         - Si no hay slots vacíos, el hilo se BLOQUEA AQUÍ
 *         - Esto causa BACKPRESSURE: el pipe no se lee → se llena
 *           → el sensor se bloquea al escribir → freno natural
 *         - Si llega shutdown, despierta por el evento y sale
 *
 *      d. ESCRIBE en el buffer circular bajo el mutex
 *         - Adquiere g_hBufferMutex
 *         - Copia el evento en buffer[bufferWriteIndex]
 *         - Avanza writeIndex (circular: % capacity)
 *         - Incrementa bufferCount
 *         - Libera g_hBufferMutex
 *
 *      e. NOTIFICA al consumidor (Dispatcher)
 *         - ReleaseSemaphore(g_hSemFull) → el Dispatcher despierta
 *
 *      f. Actualiza estadísticas
 *         - InterlockedIncrement(&eventsProcessed)
 *
 *   4. Al salir del bucle:
 *      - Decrementa contador de sensores activos
 *      - Cierra el evento de lectura
 *      - Vacía buffers, desconecta el pipe, cierra el handle
 *
 * Sincronización utilizada (SIN BUSY WAITING):
 *   • ReadFile overlapped → espera bloqueante en evento del kernel
 *   • WaitForMultipleObjects en semáforo → bloquea hasta que haya slot
 *   • WaitForSingleObject en mutex → bloquea hasta tener acceso
 *   • InterlockedIncrement/Decrement → operaciones atómicas sin locks
 */
DWORD WINAPI Broker_ReceiverThread(LPVOID lpParam)
{
    HANDLE hPipe = (HANDLE)lpParam;
    TelemetryEvent event;
    BOOL connected = TRUE;

    /* Notificar al sistema que hay un sensor activo más */
    InterlockedIncrement(&g_pBuffer->activeSensors);

    /*
     * Crear evento para la E/S overlapped.
     * Este evento se señaliza cuando ReadFile completa.
     * Es manual-reset (TRUE) porque reutilizamos el mismo
     * evento en múltiples lecturas.
     */
    HANDLE hReadEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!hReadEvent) {
        InterlockedDecrement(&g_pBuffer->activeSensors);
        CloseHandle(hPipe);
        return 1;
    }

    /* Bucle principal: procesa eventos hasta shutdown o desconexión */
    while (!g_shutdown && connected) {
        /*
         * Preparar estructura OVERLAPPED para lectura asíncrona.
         * Esta estructura permite que ReadFile no bloquee el hilo,
         * y podemos usar WaitForMultipleObjects para esperar entre
         * la finalización de la lectura y la señal de shutdown.
         */
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = hReadEvent;

        /*
         * Paso (a): LECTURA ASÍNCRONA del Named Pipe.
         *
         * ReadFile con OVERLAPPED nunca bloquea:
         *   - Si retorna TRUE: completó síncronamente (datos ya disponibles)
         *   - Si retorna FALSE con ERROR_IO_PENDING: operación en curso
         *   - Si retorna FALSE con otro error: pipe desconectado
         *
         * En todos los casos (excepto error real), esperamos en
         * WaitForMultipleObjects entre hReadEvent y g_hShutdownEvent.
         */
        BOOL ok = ReadFile(hPipe, &event, sizeof(TelemetryEvent), NULL, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) {
            /* Error real: el sensor se desconectó o el pipe falló */
            connected = FALSE;
            break;
        }

        /*
         * Esperar a que la lectura complete O que llegue shutdown.
         *
         * WaitForMultipleObjects con bWaitAll = FALSE se despierta
         * cuando CUALQUIERA de los objetos está señalizado.
         *
         *   WAIT_OBJECT_0     → hReadEvent (lectura completada)
         *   WAIT_OBJECT_0 + 1 → g_hShutdownEvent (apagado)
         *
         * No hay busy-waiting: el hilo se bloquea en estado de
         * espera del kernel (0% CPU) hasta que ocurre algo.
         */
        HANDLE waitArr[2] = { hReadEvent, g_hShutdownEvent };
        DWORD wr = WaitForMultipleObjects(2, waitArr, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0 + 1) {
            /* Shutdown solicitado: cancelar la E/S pendiente y salir */
            CancelIo(hPipe);
            break;
        }

        /*
         * Obtener el resultado de la operación overlapped.
         * GetOverlappedResult espera si es necesario (bWait = FALSE
         * no espera porque el evento ya está señalizado).
         */
        DWORD xfer = 0;
        if (!GetOverlappedResult(hPipe, &ov, &xfer, FALSE) ||
            xfer != sizeof(TelemetryEvent)) {
            connected = FALSE;
            break;
        }

        /*
         * Paso (b): ASIGNAR ID ÚNICO y TIMESTAMP.
         *
         * InterlockedIncrement garantiza atomicidad en la asignación
         * del eventId entre múltiples hilos receptores.
         *
         * QueryPerformanceCounter proporciona una marca de tiempo
         * de alta resolución (nanosegundos en hardware moderno).
         */
        event.eventId = (DWORD)InterlockedIncrement(&g_nextEventId);
        QueryPerformanceCounter(&event.timestamp);

        /*
         * Paso (c): ESPERAR SLOT VACÍO.
         *
         * g_hSemEmpty cuenta los slots disponibles para escribir.
         * Si el buffer está lleno, este semáforo tiene count = 0
         * y WaitForMultipleObjects BLOQUEA EL HILO.
         *
         * ESTO ES BACKPRESSURE:
         *   • El hilo no lee del pipe → el buffer interno del pipe
         *     se llena (PIPE_BUFFER_SIZE = 4096 bytes)
         *   • El sensor (que escribe con WriteFile bloqueante) se
         *     bloquea porque el pipe está lleno
         *   • El sistema se autorregula SIN PERDER DATOS
         *
         * Nuevamente incluimos g_hShutdownEvent para poder
         * despertar durante el apagado.
         */
        /*
         * Paso (c): ESPERAR SLOT VACÍO con timeout.
         *
         * Usamos timeout de 100ms para detectar pipes desconectados
         * mientras el buffer está lleno (backpressure). Si el sensor
         * cierra la conexión mientras esperamos, detectamos el pipe
         * roto y salimos sin perder el hilo.
         *
         * 100ms de espera kernel NO es busy-waiting (0% CPU).
         * Cuando hay backpressure, el sensor se bloquea al escribir
         * y NO cierra la conexión, así que en condiciones normales
         * semEmpty se libera cuando el Dispatcher consume eventos.
         */
        DWORD sr;
        do {
            HANDLE ws[2] = { g_hSemEmpty, g_hShutdownEvent };
            sr = WaitForMultipleObjects(2, ws, FALSE, 100);
            if (sr == WAIT_TIMEOUT) {
                DWORD state = 0;
                if (!GetNamedPipeHandleStateA(hPipe, &state, NULL, NULL, NULL, NULL, 0)) {
                    connected = FALSE;
                    break;
                }
            }
        } while (sr == WAIT_TIMEOUT && connected);
        if (!connected) break;
        if (sr == WAIT_OBJECT_0 + 1) break;

        /*
         * Paso (d): ESCRIBIR EN EL BUFFER CIRCULAR.
         *
         * Adquirimos el mutex para acceso exclusivo a los índices
         * del buffer. El mutex se libera inmediatamente después
         * de la escritura (sección crítica muy breve).
         *
         * El buffer es circular: cuando writeIndex llega al final,
         * vuelve al principio (operación módulo).
         */
        WaitForSingleObject(g_hBufferMutex, INFINITE);

        g_pBuffer->buffer[g_pBuffer->bufferWriteIndex] = event;
        g_pBuffer->bufferWriteIndex =
            (g_pBuffer->bufferWriteIndex + 1) % g_pBuffer->bufferCapacity;
        g_pBuffer->bufferCount++;

        ReleaseMutex(g_hBufferMutex);

        /*
         * Paso (e): NOTIFICAR AL CONSUMIDOR.
         *
         * ReleaseSemaphore incrementa g_hSemFull en 1.
         * Esto despierta al Dispatcher (Módulo 3) que está
         * bloqueado esperando datos en el buffer.
         *
         * El tercer parámetro (opcional) recibe el valor anterior
         * del semáforo; nosotros pasamos NULL porque no nos interesa.
         */
        ReleaseSemaphore(g_hSemFull, 1, NULL);

        /*
         * Paso (f): ACTUALIZAR ESTADÍSTICAS.
         * Evento procesado exitosamente.
         */
        InterlockedIncrement(&g_pBuffer->eventsProcessed);
    }

    /* --- LIMPIEZA DEL HILO --- */

    /* Decrementar contador de sensores activos */
    InterlockedDecrement(&g_pBuffer->activeSensors);

    /* Cerrar evento de lectura overlapped */
    CloseHandle(hReadEvent);

    /*
     * Cerrar el pipe ordenadamente:
     *   1. FlushFileBuffers: asegura que todos los datos pendientes
     *      se escriban antes de desconectar
     *   2. DisconnectNamedPipe: cierra la conexión del lado del servidor
     *      (el cliente detecta ERROR_BROKEN_PIPE)
     *   3. CloseHandle: libera el handle del pipe
     */
    FlushFileBuffers(hPipe);
    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);

    return 0;
}


/* ===================================================================
 * Broker_Run - Bucle principal de aceptación de conexiones
 * ===================================================================
 *
 * Este es el núcleo del broker. Ejecuta un bucle infinito que:
 *
 *   1. Crea una instancia del Named Pipe (CreateNamedPipeA)
 *      - Usa PIPE_ACCESS_DUPLEX (lectura/escritura)
 *      - FILE_FLAG_OVERLAPPED para operaciones asíncronas
 *      - PIPE_TYPE_MESSAGE: pipe orientado a mensajes (no a bytes)
 *      - PIPE_READMODE_MESSAGE: lectura por mensajes completos
 *      - PIPE_UNLIMITED_INSTANCES: hasta 255 instancias simultáneas
 *
 *   2. Espera una conexión entrante (ConnectNamedPipe)
 *      - También overlapped para poder despertar con shutdown
 *      - Si shutdown ocurre mientras esperamos: CancelIo y salimos
 *      - Si un cliente ya estaba esperando (ERROR_PIPE_CONNECTED):
 *        lo tratamos como conexión exitosa
 *
 *   3. Lanza un hilo receptor (CreateThread)
 *      - El hilo toma posesión del handle del pipe
 *      - Cerramos el handle del hilo (no necesitamos esperarlo)
 *
 *   4. Vuelve al paso 1 para aceptar más sensores
 *
 * Nota: PIPE_UNLIMITED_INSTANCES permite que múltiples sensores
 * se conecten simultáneamente. Cada conexión recibe su propio
 * hilo y su propia instancia del pipe.
 */
void Broker_Run(void)
{
    /* Registrar el manejador de consola para Ctrl+C */
    SetConsoleCtrlHandler(Broker_ConsoleHandler, TRUE);

    printf("=== Telemetry Broker Ingestor Central ===\n");
    printf("Waiting for sensor connections on %s ...\n", PIPE_NAME);
    printf("Press Ctrl+C to stop.\n\n");

    while (!g_shutdown) {
        /*
         * Paso 1: Crear una instancia del Named Pipe.
         *
         * Cada llamada a CreateNamedPipe con el mismo nombre crea
         * una NUEVA INSTANCIA del pipe. Así podemos atender
         * múltiples clientes simultáneamente.
         *
         * PIPE_BUFFER_SIZE = 4096 bytes para los buffers de
         * entrada y salida del pipe.
         */
        HANDLE hPipe = CreateNamedPipeA(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUFFER_SIZE,
            PIPE_BUFFER_SIZE,
            0,
            NULL);

        if (hPipe == INVALID_HANDLE_VALUE) {
            if (!g_shutdown) Sleep(200);
            continue;
        }

        /*
         * Paso 2: Esperar conexión entrante (overlapped).
         *
         * Usamos OVERLAPPED + evento para poder detectar shutdown
         * mientras esperamos una conexión. Sin overlapped,
         * ConnectNamedPipe bloquearía indefinidamente y no
         * podríamos salir con Ctrl+C.
         */
        OVERLAPPED cov;
        ZeroMemory(&cov, sizeof(cov));
        HANDLE hConnectEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!hConnectEvent) {
            CloseHandle(hPipe);
            continue;
        }
        cov.hEvent = hConnectEvent;

        BOOL connected = ConnectNamedPipe(hPipe, &cov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                /* Conexión en progreso: esperar con shutdown */
                HANDLE cw[2] = { hConnectEvent, g_hShutdownEvent };
                DWORD cwr = WaitForMultipleObjects(2, cw, FALSE, INFINITE);
                if (cwr == WAIT_OBJECT_0 + 1) {
                    /* Shutdown durante la espera de conexión */
                    CancelIo(hPipe);
                    CloseHandle(hConnectEvent);
                    CloseHandle(hPipe);
                    break;
                }
                /* Cliente conectado exitosamente */
                DWORD dummy = 0;
                connected = GetOverlappedResult(hPipe, &cov, &dummy, FALSE);
            } else if (err == ERROR_PIPE_CONNECTED) {
                /*
                 * El cliente se conectó entre CreateNamedPipe
                 * y ConnectNamedPipe (race condition benigna).
                 * Se considera conexión exitosa.
                 */
                connected = TRUE;
            }
        }
        CloseHandle(hConnectEvent);

        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        /*
         * Paso 3: Lanzar hilo receptor para este sensor.
         *
         * El hilo recibe el handle del pipe como parámetro
         * y se encarga de toda la comunicación con ese sensor.
         *
         * Cerramos el handle del hilo inmediatamente (no lo
         * necesitamos para Join). El hilo sigue ejecutándose
         * independientemente y se limpiará solo cuando el
         * sensor se desconecte o llegue shutdown.
         */
        printf("[+] Sensor connected, spawning receiver thread...\n");

        HANDLE hThread = CreateThread(NULL, 0, Broker_ReceiverThread,
                                       (LPVOID)hPipe, 0, NULL);
        if (!hThread) {
            printf("[-] Failed to create thread, disconnecting.\n");
            DisconnectNamedPipe(hPipe);
            CloseHandle(hPipe);
            continue;
        }
        CloseHandle(hThread);
    }

    /* Llegó shutdown: esperar un momento y limpiar */
    printf("\nShutdown signal received. Waiting for threads to finish...\n");
    Sleep(2000);
    Broker_Shutdown();
    printf("Broker shutdown complete.\n");
}


/* ===================================================================
 * main - Punto de entrada del proceso Broker
 * ===================================================================
 *
 * 1. Desactiva el buffering de stdout (setvbuf con _IONBF)
 *    para que los mensajes se vean inmediatamente, incluso
 *    cuando la salida está redirigida a un archivo.
 *
 * 2. Inicializa el broker (Broker_Init).
 *    Si falla, muestra el error y termina con código 1.
 *
 * 3. Ejecuta el bucle principal (Broker_Run).
 *    No retorna hasta que ocurre Ctrl+C.
 *
 * 4. Termina con código 0 (éxito).
 */
int main(void)
{
    /* stdout sin buffer para visualización en tiempo real */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (!Broker_Init()) {
        printf("Broker initialization failed.\n");
        return 1;
    }

    Broker_Run();
    return 0;
}
