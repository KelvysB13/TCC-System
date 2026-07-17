// Creador: Kelvys Concepcion
// Ultima Modificacion: 16/07/2026
// Descripcion: Colas prioritarias, workers y persistencia con LockFileEx.

#include "processor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP_COL CAP_COLA

typedef struct {

    Evento       items[CAP_COL];
    volatile LONG cabeza;
    volatile LONG cola;
    volatile LONG cnt;

} ColaPrio;

static struct {

    ColaPrio        c;
    CRITICAL_SECTION cs;

} g_colas[N_PRIOR];

static HANDLE  g_hParar;       // Evento: señal de detención.
static HANDLE  g_hTrabajo;     // Semáforo: trabajo disponible.
static HANDLE  g_hTrabajadores[N_TRABAJ];
static HANDLE  g_hLog;
static CRITICAL_SECTION g_csLog;

// Convierte tipo de sensor a cadena
static const char *tipo_str(SENSOR_TYPE t)
{
    switch (t) 
    {
    case SENSOR_TYPE_ENGINE: return "ENGINE";
    case SENSOR_TYPE_TIRES:  return "TIRES";
    case SENSOR_TYPE_BRAKES: return "BRAKES";
    case SENSOR_TYPE_GPS:    return "GPS";
    default:                 return "UNKN";
    }
}

// Convierte prioridad a cadena.
static const char *prio_str(Prioridad p)
{
    switch (p) 
    {
    case CRIT: return "CRIT";
    case ALTA: return "ALTA";
    case NORM: return "NORM";
    case BAJA: return "BAJA";
    default:   return "UNKN";
    }
}

// Escribe una línea de resultado al archivo de log con LockFileEx.
static void log_escribir(Evento *ev, DWORD tid)
{
    EnterCriticalSection(&g_csLog);

    LARGE_INTEGER pos;
    pos.QuadPart = 0;
    GetFileSizeEx(g_hLog, &pos);

    char linea[256];
    DWORD n = (DWORD)snprintf(linea, sizeof(linea), "%lu|%lu|%s|%s|%llu|%lu|OK\n",
                               ev->idSensor, ev->idEvento, tipo_str(ev->tipo),
                               prio_str(ev->prio), ev->ts, tid);

    OVERLAPPED ov = {0};
    ov.Offset     = pos.LowPart;
    ov.OffsetHigh = pos.HighPart;

    LockFileEx(g_hLog, LOCKFILE_EXCLUSIVE_LOCK, 0, n, 0, &ov);
    SetFilePointer(g_hLog, pos.LowPart, &pos.HighPart, FILE_BEGIN);
    WriteFile(g_hLog, linea, n, &n, NULL);
    UnlockFileEx(g_hLog, 0, n, 0, &ov);

    LeaveCriticalSection(&g_csLog);
}

// Bucle del trabajador: toma eventos de colas prioritarias y los procesa.
static DWORD WINAPI bucle_trabaj(LPVOID arg)
{
    (void)arg;
    DWORD tid = GetCurrentThreadId();

    while (1) 
    {
        int encontro = 0;

        for (int p = CRIT; p <= BAJA; p++) 
        {
            EnterCriticalSection(&g_colas[p].cs);

            if (g_colas[p].c.cnt > 0) 
            {
                Evento ev = g_colas[p].c.items[g_colas[p].c.cola % CAP_COL];
                g_colas[p].c.cola++;
                g_colas[p].c.cnt--;
                encontro = 1;
                LeaveCriticalSection(&g_colas[p].cs);

                Sleep(50);
                log_escribir(&ev, tid);
                break;
            }

            LeaveCriticalSection(&g_colas[p].cs);
        }

        if (!encontro) 
        {
            HANDLE espera[2] = { g_hParar, g_hTrabajo };
            DWORD ret = WaitForMultipleObjects(2, espera, FALSE, INFINITE);

            if (ret == WAIT_OBJECT_0)
            { 
                break;
            }
        }
    }

    return 0;
}

// Inicializa colas, workers, semáforo de trabajo y archivo de log.
int proc_iniciar(void)
{
    InitializeCriticalSection(&g_csLog);

    for (int i = 0; i < N_PRIOR; i++) 
    {
        InitializeCriticalSection(&g_colas[i].cs);
        g_colas[i].c.cabeza = g_colas[i].c.cola = g_colas[i].c.cnt = 0;
    }

    g_hParar   = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hTrabajo = CreateSemaphoreW(NULL, 0, LONG_MAX, NULL);

    if (!g_hParar || !g_hTrabajo) 
    {
        proc_apagar();
        return -1;
    }

    char ruta[MAX_PATH];
    GetModuleFileNameA(NULL, ruta, MAX_PATH);
    char *sep = strrchr(ruta, '\\');
    if (sep) *sep = '\0';
    sep = strrchr(ruta, '\\');
    if (sep) *sep = '\0';
    strcat(ruta, "\\logs");
    CreateDirectoryA(ruta, NULL);
    strcat(ruta, "\\dispatcher.log");
    g_hLog = CreateFileA(ruta, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (g_hLog == INVALID_HANDLE_VALUE) 
    {
        proc_apagar();
        return -1;
    }

    for (int i = 0; i < N_TRABAJ; i++) 
    {
        g_hTrabajadores[i] = CreateThread(NULL, 0, bucle_trabaj, NULL, 0, NULL);

        if (!g_hTrabajadores[i]) 
        {
            proc_apagar();
            return -1;
        }
    }

    return 0;
}

// Encola un evento en la cola correspondiente a su prioridad.
int proc_enviar(Evento *ev)
{
    int p = ev->prio;
    if (p < CRIT || p > BAJA) p = NORM;

    EnterCriticalSection(&g_colas[p].cs);

    if (g_colas[p].c.cnt >= CAP_COL) 
    {
        LeaveCriticalSection(&g_colas[p].cs);
        return -1;
    }

    g_colas[p].c.items[g_colas[p].c.cabeza % CAP_COL] = *ev;
    g_colas[p].c.cabeza++;
    g_colas[p].c.cnt++;
    LeaveCriticalSection(&g_colas[p].cs);

    ReleaseSemaphore(g_hTrabajo, 1, NULL);
        return 0;
}

// Detiene todos los workers y libera los recursos del procesador.
void proc_apagar(void)
{
    if (g_hParar) SetEvent(g_hParar);

    for (int i = 0; i < N_TRABAJ; i++) 
    {
        if (g_hTrabajadores[i]) 
        {
            WaitForSingleObject(g_hTrabajadores[i], 5000);
            CloseHandle(g_hTrabajadores[i]);
            g_hTrabajadores[i] = NULL;
        }
    }

    for (int i = 0; i < N_PRIOR; i++) 
    {
        DeleteCriticalSection(&g_colas[i].cs);
    }

    if (g_hParar)   { CloseHandle(g_hParar);   g_hParar = NULL; }
    if (g_hTrabajo) { CloseHandle(g_hTrabajo); g_hTrabajo = NULL; }
    if (g_hLog && g_hLog != INVALID_HANDLE_VALUE) { CloseHandle(g_hLog); g_hLog = NULL; }

    DeleteCriticalSection(&g_csLog);
}
