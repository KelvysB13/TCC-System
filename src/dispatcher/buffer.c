// Creador: Kelvys Concepcion
// Ultima Modificacion: 16/07/2026
// Descripcion: Buffer circular sobre Memoria Compartida IPC (File Mapping)
//              sincronizado con semaforos named y mutex del Broker (M2).

#include "buffer.h"
#include "../../include/ipc_protocol.h"
#include <stdio.h>

static SHARED_BUFFER* g_pShared  = NULL; /* Puntero a la memoria compartida */
static HANDLE         g_hMapFile = NULL; /* Handle del File Mapping */
static HANDLE         g_hVacio   = NULL; /* Sem. named: slots libres (Broker) */
static HANDLE         g_hLleno   = NULL; /* Sem. named: slots ocupados (Broker) */
static HANDLE         g_hMutex   = NULL; /* Mutex named: exclusion mutua (Broker) */

// Inicializa la conexion a la memoria compartida del Broker.
// Abre el File Mapping, los semaforos named y el mutex creados por M2.
int buf_iniciar(void)
{
    /* Abrir el File Mapping creado por el Broker */
    g_hMapFile = OpenFileMappingA(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        TCC_SHARED_MEM_NAME
    );
    if (!g_hMapFile) {
        fprintf(stderr, "ERROR: OpenFileMappingA(%s) failed (%lu). "
                        "Broker (M2) debe ejecutarse primero.\n",
                TCC_SHARED_MEM_NAME, GetLastError());
        return -1;
    }

    /* Mapear la memoria compartida en el espacio de este proceso */
    g_pShared = (SHARED_BUFFER*)MapViewOfFile(
        g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0
    );
    if (!g_pShared) {
        fprintf(stderr, "ERROR: MapViewOfFile failed (%lu)\n", GetLastError());
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return -1;
    }

    /* Abrir semaforo named de slots libres (creado por Broker) */
    g_hVacio = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, TCC_SEM_EMPTY_NAME);
    if (!g_hVacio) {
        fprintf(stderr, "ERROR: OpenSemaphoreA(%s) failed (%lu)\n",
                TCC_SEM_EMPTY_NAME, GetLastError());
        buf_destruir();
        return -1;
    }

    /* Abrir semaforo named de slots ocupados (creado por Broker) */
    g_hLleno = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, TCC_SEM_FULL_NAME);
    if (!g_hLleno) {
        fprintf(stderr, "ERROR: OpenSemaphoreA(%s) failed (%lu)\n",
                TCC_SEM_FULL_NAME, GetLastError());
        buf_destruir();
        return -1;
    }

    /* Abrir mutex named (creado por Broker) */
    g_hMutex = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, TCC_MUTEX_NAME);
    if (!g_hMutex) {
        fprintf(stderr, "ERROR: OpenMutexA(%s) failed (%lu)\n",
                TCC_MUTEX_NAME, GetLastError());
        buf_destruir();
        return -1;
    }

    return 0;
}

// Lee un evento del buffer circular en memoria compartida.
// Bloquea en el semaforo de slots ocupados si el buffer esta vacio
// (espera pasiva del kernel, sin busy waiting).
int buf_leer(Evento *ev)
{
    if (!ev || !g_pShared) return -1;

    /* Esperar a que haya datos disponibles (bloqueante, pasivo) */
    if (WaitForSingleObject(g_hLleno, INFINITE) != WAIT_OBJECT_0) return -1;
    /* Exclusion mutua con el Broker (M2) y otros lectores */
    if (WaitForSingleObject(g_hMutex, INFINITE) != WAIT_OBJECT_0) return -1;

    /* Leer evento del buffer circular compartido */
    *ev = g_pShared->items[g_pShared->cola % CAP_BUF];
    g_pShared->cola++;
    g_pShared->cnt--;

    ReleaseMutex(g_hMutex);
    /* Notificar al Broker que hay un slot libre mas */
    ReleaseSemaphore(g_hVacio, 1, NULL);

    return 0;
}

// Escribe un evento al buffer circular en memoria compartida.
// Bloquea en el semaforo de slots libres si el buffer esta lleno.
// Usado internamente (el writer principal es el Broker M2).
int buf_escribir(Evento *ev)
{
    if (!ev || !g_pShared) return -1;

    if (WaitForSingleObject(g_hVacio, INFINITE) != WAIT_OBJECT_0) return -1;
    if (WaitForSingleObject(g_hMutex, INFINITE) != WAIT_OBJECT_0) return -1;

    g_pShared->items[g_pShared->cabeza % CAP_BUF] = *ev;
    g_pShared->cabeza++;
    g_pShared->cnt++;

    /* Registrar ocupacion maxima historica (para el Monitor M4) */
    if (g_pShared->cnt > g_pShared->dwBufferMaxOccupancy) {
        g_pShared->dwBufferMaxOccupancy = g_pShared->cnt;
    }

    ReleaseMutex(g_hMutex);
    ReleaseSemaphore(g_hLleno, 1, NULL);

    InterlockedIncrement(&g_pShared->dwEventsReceived);

    return 0;
}

// Libera todos los recursos: cierra handles del File Mapping,
// semaforos y mutex. No deja fugas de handles del kernel.
void buf_destruir(void)
{
    if (g_hVacio) { CloseHandle(g_hVacio); g_hVacio = NULL; }
    if (g_hLleno) { CloseHandle(g_hLleno); g_hLleno = NULL; }
    if (g_hMutex) { CloseHandle(g_hMutex); g_hMutex = NULL; }

    if (g_pShared) {
        UnmapViewOfFile(g_pShared);
        g_pShared = NULL;
    }

    if (g_hMapFile) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }
}
