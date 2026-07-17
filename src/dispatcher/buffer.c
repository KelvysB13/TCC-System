// Creador: Kelvys Concepcion
// Ultima Modificacion: 16/07/2026
// Descripcion: Buffer circular con semaforos y generador de datos mock.

#include "buffer.h"
#include <stdlib.h>
#include <stdio.h>

static BufCirc  g_buf;
static HANDLE   g_hVacio;   // Semáforo: contador de slots libres */
static HANDLE   g_hLleno;   // Semáforo: contador de slots ocupados */
static HANDLE   g_hMutex;   // Mutex: acceso exclusivo al buffer */
static HANDLE   g_hSimul;   // Thread del generador simulado */
static volatile LONG g_correr = 1;
static LONGLONG g_freq;     // Frecuencia QPC para conversion a microsegundos

// Genera un entero aleatorio en [lo, hi].
static DWORD gen_rango(DWORD lo, DWORD hi)
{
    return lo + (DWORD)((hi - lo + 1) * (rand() / (RAND_MAX + 1.0)));
}

// Llena un evento con datos de sensor aleatorios y realistas.
static void gen_ev_aleat(Evento *ev)
{
    static DWORD cont = 0;
    LARGE_INTEGER qpc;
    ev->idSensor  = (DWORD)(1 + rand() % 10);  // Simula 10 instancias de sensor
    ev->idEvento  = ++cont;                     // Secuencia global de eventos
    ev->tipo      = (SENSOR_TYPE)(1 + rand() % 4);
    ev->prio      = (Prioridad)(rand() % 4);
    QueryPerformanceCounter(&qpc);
    ev->ts        = (ULONGLONG)((qpc.QuadPart * 1000000LL) / g_freq);

    switch (ev->tipo)
    {
        case SENSOR_TYPE_ENGINE:

            ev->datos[0] = (int)gen_rango(2000, 12000);
            ev->datos[1] = (int)gen_rango(80, 120);
            ev->datos[2] = (int)gen_rango(20, 80);
            ev->datos[3] = (int)gen_rango(0, 100);
            break;

        case SENSOR_TYPE_TIRES:

            ev->datos[0] = (int)gen_rango(2800, 3600);
            ev->datos[1] = (int)gen_rango(60, 110);
            ev->datos[2] = (int)gen_rango(0, 100);
            ev->datos[3] = (int)gen_rango(0, 350);
            break;

        case SENSOR_TYPE_BRAKES:

            ev->datos[0] = (int)gen_rango(100, 800);
            ev->datos[1] = (int)gen_rango(0, 200);
            ev->datos[2] = (int)gen_rango(0, 100);
            ev->datos[3] = (int)gen_rango(0, 100);
            break;

        case SENSOR_TYPE_GPS:

            ev->datos[0] = (int)gen_rango(0, 1800) - 900;
            ev->datos[1] = (int)gen_rango(0, 3600) - 1800;
            ev->datos[2] = (int)gen_rango(0, 350);
            ev->datos[3] = (int)gen_rango(0, 359);
            break;
    }
}

// Bucle del generador simulado: crea eventos y los escribe al buffer.
static DWORD WINAPI bucle_simul(LPVOID arg)
{
    (void)arg;

    while (g_correr) 
    {
        Evento ev;
        gen_ev_aleat(&ev);
        buf_escribir(&ev);
        Sleep(50 + rand() % 100);
    }

    return 0;
}

// Inicializa el buffer circular, semáforos, mutex y el hilo generador.
int buf_iniciar(void)
{
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    g_freq = li.QuadPart;

    g_hVacio = CreateSemaphoreW(NULL, CAP_BUF, CAP_BUF, NULL);
    g_hLleno = CreateSemaphoreW(NULL, 0, CAP_BUF, NULL);
    g_hMutex = CreateMutexW(NULL, FALSE, NULL);

    if (!g_hVacio || !g_hLleno || !g_hMutex) 
    {
        if (g_hVacio) CloseHandle(g_hVacio);
        if (g_hLleno) CloseHandle(g_hLleno);
        if (g_hMutex) CloseHandle(g_hMutex);

        return -1;
    }

    g_buf.cabeza = g_buf.cola = g_buf.cnt = 0;

    g_hSimul = CreateThread(NULL, 0, bucle_simul, NULL, 0, NULL);

    if (!g_hSimul) 
    { 
        buf_destruir(); 
        return -1; 
    }

    return 0;
}

// Lee un evento del buffer circular (bloquea si está vacío).
int buf_leer(Evento *ev)
{
    if (WaitForSingleObject(g_hLleno, INFINITE) != WAIT_OBJECT_0) return -1;
    if (WaitForSingleObject(g_hMutex, INFINITE) != WAIT_OBJECT_0) return -1;

    *ev = g_buf.items[g_buf.cola % CAP_BUF];
    g_buf.cola++;
    g_buf.cnt--;

    ReleaseMutex(g_hMutex);
    ReleaseSemaphore(g_hVacio, 1, NULL);

    return 0;
}

// Escribe un evento al buffer circular (bloquea si está lleno).
int buf_escribir(Evento *ev)
{
    if (WaitForSingleObject(g_hVacio, INFINITE) != WAIT_OBJECT_0) return -1;
    if (WaitForSingleObject(g_hMutex, INFINITE) != WAIT_OBJECT_0) return -1;

    g_buf.items[g_buf.cabeza % CAP_BUF] = *ev;
    g_buf.cabeza++;
    g_buf.cnt++;

    ReleaseMutex(g_hMutex);
    ReleaseSemaphore(g_hLleno, 1, NULL);

    return 0;
}

// Libera todos los recursos del buffer y detiene el generador.
void buf_destruir(void)
{
    g_correr = 0;

    if (g_hSimul) 
    {
        ReleaseSemaphore(g_hVacio, 1, NULL);
        WaitForSingleObject(g_hSimul, 1000);
        CloseHandle(g_hSimul);
        g_hSimul = NULL;
    }
    
    if (g_hVacio) { CloseHandle(g_hVacio); g_hVacio = NULL; }
    if (g_hLleno) { CloseHandle(g_hLleno); g_hLleno = NULL; }
    if (g_hMutex) { CloseHandle(g_hMutex); g_hMutex = NULL; }
}
