/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 * Modulo 4  - Monitor del Sistema y Dashboard de Control
 *
 * Desarrollador: Miguel Mora
 *
 * monitor.c - Implementacion del proceso Monitor independiente.
 *
 * PROPOSITO:
 *   Consola de administracion del sistema en tiempo real. Se ejecuta como
 *   proceso completamente separado del Broker (M2) y del Dispatcher (M3).
 *   Abre la memoria compartida en modo SOLO LECTURA para extraer estadisticas
 *   y se coordina con el Broker mediante Eventos de Windows (sin senales UNIX).
 *
 * MECANISMO IPC:
 *   - OpenFileMappingA / MapViewOfFile: acceder a SHARED_BUFFER (solo lectura)
 *   - OpenEventA (TCC_ShutdownEvent):  senalizar apagado ordenado del sistema
 *   - OpenEventA (TCC_DebugEvent):     alternar el debug dump del Broker
 *   NO se usan busy-waiting, senales UNIX ni librerias externas de hilos.
 *
 * COMANDOS INTERACTIVOS (teclas, no bloqueantes mediante _kbhit):
 *   [D] - Toggle debug dump en el Broker via TCC_DebugEvent
 *   [S] - Apagar todo el sistema via TCC_ShutdownEvent
 *   [Q] - Cerrar solo el monitor de forma limpia
 *
 * CIERRE LIMPIO DE HANDLES:
 *   Antes de salir, siempre se llama a:
 *     UnmapViewOfFile  -> liberar la vista de memoria
 *     CloseHandle      -> cerrar handles de File Mapping y Eventos
 *   Garantia absoluta de ausencia de fugas de handles en el kernel.
 */

#include "monitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>   /* _kbhit, _getch: lectura de teclado no bloqueante (Win32) */

/* ====================================================================
 *  ESTADO GLOBAL DEL MONITOR
 *
 *  Variables static: visibles solo dentro de este archivo.
 *  Los handles se inicializan a NULL para poder verificar
 *  su validez antes de cerrarlos en la funcion de limpieza.
 * ==================================================================== */

/* Handle del File Mapping abierto en modo lectura */
static HANDLE          g_hMapFile      = NULL;

/* Vista mapeada de la memoria compartida (puntero a SHARED_BUFFER).
 * Se abre con FILE_MAP_READ: este proceso NUNCA escribe en el buffer
 * compartido; solo lee estadisticas de forma segura. */
static const SHARED_BUFFER* g_pBuf     = NULL;

/* Handle del evento de apagado: TCC_ShutdownEvent.
 * El Monitor llama SetEvent(g_hShutdownEvt) para ordenar al Broker
 * que inicie su protocolo de parada controlada. */
static HANDLE          g_hShutdownEvt  = NULL;

/* Handle del evento de debug: TCC_DebugEvent.
 * El Monitor llama SetEvent(g_hDebugEvt) para alternar el
 * volcado de depuracion en la consola del Broker (M2). */
static HANDLE          g_hDebugEvt     = NULL;

/* Bandera de ejecucion del bucle principal del monitor.
 * volatile: puede ser modificada por el manejador Ctrl+C (otro hilo del OS). */
static volatile BOOL   g_bRunning      = TRUE;

/* Bandera que indica si los colores ANSI estan disponibles en este terminal */
static BOOL            g_bAnsiOk       = FALSE;

/* ====================================================================
 *  FUNCION: Monitor_SensorTypeStr
 *
 *  Convierte el codigo numerico de tipo de sensor a su cadena legible.
 *  Centralizado aqui para no depender de funciones de otros modulos.
 * ==================================================================== */
const char*
Monitor_SensorTypeStr(DWORD dwType)
{
    switch (dwType) {
    case SENSOR_TYPE_ENGINE: return "Engine";
    case SENSOR_TYPE_TIRES:  return "Tires ";
    case SENSOR_TYPE_BRAKES: return "Brakes";
    case SENSOR_TYPE_GPS:    return "GPS   ";
    default:                 return "??    ";
    }
}

/* ====================================================================
 *  FUNCION: Monitor_HabilitarAnsi
 *
 *  Activa ENABLE_VIRTUAL_TERMINAL_PROCESSING en el handle de salida
 *  estandar. Esto permite que Windows interprete los codigos de escape
 *  ANSI (\x1b[...) como colores y movimientos de cursor en lugar de
 *  mostrarlos como texto literal.
 *
 *  Disponible desde Windows 10 Anniversary Update (build 14393).
 *  En terminales mas antiguos, el dashboard se muestra sin colores.
 * ==================================================================== */
BOOL
Monitor_HabilitarAnsi(void)
{
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return FALSE;

    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return FALSE;

    /* Agregar la bandera de procesamiento de secuencias virtuales */
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return SetConsoleMode(hOut, dwMode);
}

/* ====================================================================
 *  FUNCION: Monitor_DibujarBarra
 *
 *  Dibuja una barra de progreso horizontal estilo ASCII en la consola.
 *  Ejemplo con valor=25, maximo=64, ancho=40:
 *    [###############.........................]  39%
 *
 *  La barra se colorea segun el nivel de llenado:
 *    < 50%  -> verde  (sistema saludable)
 *    < 75%  -> amarillo (carga moderada)
 *    >= 75% -> rojo   (riesgo de desbordamiento / backpressure activo)
 * ==================================================================== */
void
Monitor_DibujarBarra(LONG valor, LONG maximo, int ancho)
{
    /* Proteccion contra division por cero */
    if (maximo <= 0) {
        printf("[");
        for (int i = 0; i < ancho; i++) printf(".");
        printf("]  N/A");
        return;
    }

    /* Calcular cuantos caracteres de la barra se deben rellenar */
    int relleno = (int)((valor * ancho) / maximo);
    if (relleno > ancho) relleno = ancho;
    int vacio   = ancho - relleno;

    /* Calcular porcentaje para mostrar y elegir color */
    int pct = (int)((valor * 100) / maximo);

    /* Seleccionar color segun nivel de ocupacion */
    const char* colorRelleno = ANSI_GREEN;
    if (pct >= 75) {
        colorRelleno = ANSI_RED;
    } else if (pct >= 50) {
        colorRelleno = ANSI_YELLOW;
    }

    /* Dibujar la barra: "[" + parte llena + parte vacia + "]  pct%" */
    printf("%s[%s", ANSI_DIM ANSI_WHITE, colorRelleno);
    for (int i = 0; i < relleno; i++) printf("\xe2\x96\x88"); /* Bloque solido Unicode */
    printf("%s", ANSI_DIM);
    for (int i = 0; i < vacio;   i++) printf(".");
    printf("%s]%s  %s%3d%%%s",
           ANSI_WHITE, ANSI_RESET,
           colorRelleno, pct, ANSI_RESET);
}

/* ====================================================================
 *  FUNCION: Monitor_DibujarDashboard
 *
 *  Renderiza el panel de control completo en la consola. Se llama
 *  periodicamente desde el bucle principal (cada MONITOR_REFRESH_MS ms).
 *
 *  Diseño del dashboard:
 *   ┌─ Cabecera con titulo y hora actual
 *   ├─ Panel de estadisticas globales
 *   │    Eventos totales, sensores activos, ocupacion del buffer
 *   │    Barra de progreso visual del buffer circular
 *   ├─ Tabla de sensores activos (ID, tipo, estado)
 *   └─ Panel de comandos disponibles
 *
 *  Nota sobre concurrencia: la lectura del SHARED_BUFFER es de solo
 *  lectura y los campos son volatile LONG, lo que garantiza lecturas
 *  atomicas en x86. No se necesita mutex adicional para el monitor.
 * ==================================================================== */
void
Monitor_DibujarDashboard(const SHARED_BUFFER* pBuf)
{
    /* Leer todos los valores en variables locales de una vez,
     * para que el dashboard muestre un snapshot coherente */
    LONG lEventos  = pBuf->dwEventsReceived;
    LONG lActivos  = pBuf->dwActiveSensors;
    LONG lCnt      = pBuf->cnt;
    LONG lMaxOcc   = pBuf->dwBufferMaxOccupancy;

    /* Obtener hora del sistema para mostrarla en la cabecera */
    SYSTEMTIME st;
    GetLocalTime(&st);

    /* Limpiar la pantalla completamente antes de redibujar el dashboard.
     * system("cls") invoca el comando nativo de Windows que borra la
     * consola, evitando artefactos visuales por sobreescritura. */
    system("cls");

    /* ── CABECERA ──────────────────────────────────────────────── */
    printf("%s%s", ANSI_BG_BLACK, ANSI_BOLD);
    printf("%s", ANSI_CYAN);
    printf("  ╔══════════════════════════════════════════════════════════╗\n");
    printf("  ║       %s TCC-System  |  Monitor del Sistema  M4 %s       ║\n",
           ANSI_YELLOW, ANSI_CYAN);
    printf("  ║              %sTelemetria Automotriz F1%s                   ║\n",
           ANSI_WHITE, ANSI_CYAN);
    printf("  ╚══════════════════════════════════════════════════════════╝\n");
    printf("%s\n", ANSI_RESET);

    /* Hora actual (lectura local, no depende del broker) */
    printf("  %sHora del sistema:%s  %s%02d:%02d:%02d%s\n\n",
           ANSI_DIM ANSI_WHITE, ANSI_RESET,
           ANSI_BOLD ANSI_WHITE,
           st.wHour, st.wMinute, st.wSecond,
           ANSI_RESET);

    /* ── PANEL DE ESTADISTICAS GLOBALES ────────────────────────── */
    printf("  %s┌─ ESTADISTICAS DEL SISTEMA ─────────────────────────────────┐%s\n",
           ANSI_BLUE, ANSI_RESET);

    /* Eventos totales procesados */
    printf("  %s│%s  Eventos recibidos (total)  : %s%s%ld%s\n",
           ANSI_BLUE, ANSI_RESET,
           ANSI_BOLD, ANSI_GREEN, lEventos, ANSI_RESET);

    /* Sensores activos ahora mismo */
    const char* colorActivos = (lActivos > 0) ? ANSI_GREEN : ANSI_YELLOW;
    printf("  %s│%s  Sensores activos ahora     : %s%s%ld%s\n",
           ANSI_BLUE, ANSI_RESET,
           ANSI_BOLD, colorActivos, lActivos, ANSI_RESET);

    /* Ocupacion pico historica del buffer */
    printf("  %s│%s  Ocupacion pico del buffer  : %s%s%ld / %d%s\n",
           ANSI_BLUE, ANSI_RESET,
           ANSI_BOLD, ANSI_MAGENTA,
           lMaxOcc, CAP_BUF, ANSI_RESET);

    printf("  %s└────────────────────────────────────────────────────────────┘%s\n\n",
           ANSI_BLUE, ANSI_RESET);

    /* ── BARRA DE OCUPACION DEL BUFFER CIRCULAR ────────────────── */
    printf("  %s┌─ BUFFER CIRCULAR (IPC Broker → Dispatcher) ────────────────┐%s\n",
           ANSI_BLUE, ANSI_RESET);
    printf("  %s│%s  Ocupacion actual:  %s%ld / %d%s\n",
           ANSI_BLUE, ANSI_RESET,
           ANSI_BOLD ANSI_WHITE, lCnt, CAP_BUF, ANSI_RESET);
    printf("  %s│%s  ", ANSI_BLUE, ANSI_RESET);

    /* Dibujar la barra de progreso visual */
    Monitor_DibujarBarra(lCnt, (LONG)CAP_BUF, MONITOR_BAR_WIDTH);
    printf("\n");

    /* Advertencia si el buffer esta casi lleno (backpressure activo) */
    if (lCnt >= (LONG)(CAP_BUF * 75 / 100)) {
        printf("  %s│%s  %s⚠  BACKPRESSURE ACTIVO: sensores pueden bloquearse%s\n",
               ANSI_BLUE, ANSI_RESET,
               ANSI_BOLD ANSI_RED, ANSI_RESET);
    } else {
        printf("  %s│%s  %s✓  Flujo de datos normal%s\n",
               ANSI_BLUE, ANSI_RESET,
               ANSI_GREEN, ANSI_RESET);
    }

    printf("  %s└────────────────────────────────────────────────────────────┘%s\n\n",
           ANSI_BLUE, ANSI_RESET);

    /* ── TABLA DE SENSORES CONECTADOS ──────────────────────────── */
    printf("  %s┌─ SENSORES REGISTRADOS EN EL SISTEMA ───────────────────────┐%s\n",
           ANSI_BLUE, ANSI_RESET);
    printf("  %s│%s  %-6s  %-10s  %s\n",
           ANSI_BLUE, ANSI_RESET ANSI_BOLD ANSI_WHITE,
           "Slot", "Tipo", "ID Sensor");
    printf("  %s│%s  %s─────────────────────────────────────────────────%s\n",
           ANSI_BLUE, ANSI_RESET, ANSI_DIM, ANSI_RESET);

    int nMostrados = 0;
    for (int i = 0; i < MAX_SENSOR_SLOTS; i++) {
        DWORD dwId   = pBuf->adwSensorIds[i];
        DWORD dwType = pBuf->adwSensorTypes[i];

        /* Mostrar solo slots con sensor activo (ID != 0) */
        if (dwId == 0) continue;

        /* Elegir color segun tipo de sensor */
        const char* colorTipo = ANSI_WHITE;
        switch (dwType) {
        case SENSOR_TYPE_ENGINE: colorTipo = ANSI_RED;    break;
        case SENSOR_TYPE_BRAKES: colorTipo = ANSI_YELLOW; break;
        case SENSOR_TYPE_TIRES:  colorTipo = ANSI_CYAN;   break;
        case SENSOR_TYPE_GPS:    colorTipo = ANSI_GREEN;  break;
        }

        /* Imprimir fila en segmentos para evitar ambiguedad de
         * especificadores de formato con los macros de escape ANSI */
        printf("  %s%s%s  [%02d]    ", ANSI_BLUE, "│", ANSI_RESET, i);
        printf("%s%s%-10s%s  #%lu\n",
               ANSI_BOLD, colorTipo,
               Monitor_SensorTypeStr(dwType), ANSI_RESET,
               (unsigned long)dwId);
        nMostrados++;
    }


    if (nMostrados == 0) {
        printf("  %s│%s  %sSin sensores conectados actualmente...%s\n",
               ANSI_BLUE, ANSI_RESET,
               ANSI_DIM ANSI_WHITE, ANSI_RESET);
    }

    printf("  %s└────────────────────────────────────────────────────────────┘%s\n\n",
           ANSI_BLUE, ANSI_RESET);

    /* ── PANEL DE COMANDOS ─────────────────────────────────────── */
    printf("  %s┌─ COMANDOS DE CONTROL ──────────────────────────────────────┐%s\n",
           ANSI_BLUE, ANSI_RESET);
    printf("  %s│%s  %s[D]%s  Toggle debug dump en el Broker\n",
           ANSI_BLUE, ANSI_RESET, ANSI_BOLD ANSI_CYAN, ANSI_RESET);
    printf("  %s│%s  %s[S]%s  Apagar el sistema completo (Broker + Dispatcher)\n",
           ANSI_BLUE, ANSI_RESET, ANSI_BOLD ANSI_RED, ANSI_RESET);
    printf("  %s│%s  %s[Q]%s  Cerrar solo este monitor (el sistema sigue activo)\n",
           ANSI_BLUE, ANSI_RESET, ANSI_BOLD ANSI_YELLOW, ANSI_RESET);
    printf("  %s└────────────────────────────────────────────────────────────┘%s\n\n",
           ANSI_BLUE, ANSI_RESET);

    /* Limpiar cualquier linea sucia que pueda quedar de refresco anterior */
    printf("  %sRefresco cada %d ms | Ctrl+C para salida de emergencia%s\n",
           ANSI_DIM, MONITOR_REFRESH_MS, ANSI_RESET);

    /* Forzar escritura del buffer de salida para que se vea de inmediato */
    fflush(stdout);
}

/* ====================================================================
 *  MANEJADOR DE CIERRE: CtrlHandler
 *
 *  Instalado con SetConsoleCtrlHandler. Se llama en un hilo separado
 *  del sistema operativo cuando el usuario presiona Ctrl+C, cierra
 *  la ventana, etc. Solo marca g_bRunning = FALSE para que el bucle
 *  principal termine de forma ordenada en su propia iteracion.
 *
 *  NO libera recursos aqui: eso lo hace la funcion de limpieza en main.
 * ==================================================================== */
static BOOL WINAPI
CtrlHandler(DWORD dwCtrlType)
{
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        /* Solo marcar para salida ordenada; main() hara la limpieza */
        g_bRunning = FALSE;
        return TRUE;
    default:
        return FALSE;
    }
}

/* ====================================================================
 *  FUNCION: Monitor_Limpiar
 *
 *  Libera TODOS los recursos del sistema operativo que el monitor
 *  adquirio durante su ejecucion. Obligatorio segun los requisitos:
 *    - UnmapViewOfFile: libera la vista mapeada de memoria compartida
 *    - CloseHandle: cierra el handle del File Mapping
 *    - CloseHandle: cierra handles de los Eventos de Windows
 *
 *  Se llama tanto en la salida normal como ante cualquier error.
 *  Todos los handles se verifican antes de cerrarlos (pueden ser NULL
 *  si la inicializacion fallo a medias).
 * ==================================================================== */
static void
Monitor_Limpiar(void)
{
    /* Liberar la vista mapeada de memoria compartida.
     * UnmapViewOfFile desconecta el puntero del espacio de paginas del SO. */
    if (g_pBuf != NULL) {
        UnmapViewOfFile((LPVOID)g_pBuf);
        g_pBuf = NULL;
    }

    /* Cerrar el handle del File Mapping.
     * El mapeo en si no se destruye aqui: lo destruye el Broker (M2)
     * cuando el llama a CloseHandle de su propio g_hMapFile. */
    if (g_hMapFile != NULL) {
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
    }

    /* Cerrar el handle del evento de apagado */
    if (g_hShutdownEvt != NULL) {
        CloseHandle(g_hShutdownEvt);
        g_hShutdownEvt = NULL;
    }

    /* Cerrar el handle del evento de debug */
    if (g_hDebugEvt != NULL) {
        CloseHandle(g_hDebugEvt);
        g_hDebugEvt = NULL;
    }
}

/* ====================================================================
 *  PUNTO DE ENTRADA: main
 *
 *  Secuencia de inicializacion:
 *    1. Configurar consola y titulo de ventana
 *    2. Instalar manejador de Ctrl+C
 *    3. Abrir la memoria compartida del Broker (solo lectura)
 *    4. Abrir los eventos de coordinacion (solo senalizacion)
 *    5. Bucle principal:
 *         a. Refrescar el dashboard cada MONITOR_REFRESH_MS ms
 *         b. Procesar teclas del usuario (no bloqueante)
 *    6. Limpieza ordenada de todos los handles
 *
 *  El monitor puede iniciarse ANTES o DESPUES del Broker. Si la
 *  memoria compartida no existe aun, muestra un mensaje de espera
 *  y reintenta cada segundo hasta que el Broker la cree.
 * ==================================================================== */
int
main(void)
{
    /* ── PASO 1: Configurar la consola ─────────────────────────── */

    /* Cambiar titulo de la ventana de consola */
    SetConsoleTitleA(MONITOR_WINDOW_TITLE);

    /* Activar colores ANSI: sin esto los codigos de escape se muestran
     * como texto plano en la consola de Windows */
    g_bAnsiOk = Monitor_HabilitarAnsi();

    /* Establecer codificacion UTF-8 para que los caracteres Unicode
     * (╔══╗, ┌──┐, ███, etc.) se rendericen correctamente */
    SetConsoleOutputCP(CP_UTF8);

    if (!g_bAnsiOk) {
        /* El terminal no soporta ANSI: el dashboard se ve sin colores.
         * No es un error fatal; el sistema sigue funcionando. */
        fprintf(stderr, "[MONITOR] Advertencia: ANSI no disponible. "
                        "Se usara texto plano.\n");
    }

    /* ── PASO 2: Instalar manejador de Ctrl+C ──────────────────── */
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
        fprintf(stderr, "[MONITOR] Error: SetConsoleCtrlHandler fallo (%lu)\n",
                GetLastError());
        return MONITOR_ERR_CONSOLE;
    }

    /* ── PASO 3: Abrir la memoria compartida del Broker ─────────
     *
     *  Se usa OpenFileMappingA en lugar de CreateFileMappingA porque
     *  el Broker (M2) ya creo el mapeo. El monitor es un consumidor
     *  de solo lectura: FILE_MAP_READ garantiza que nunca podamos
     *  corromper los datos del buffer circular.
     *
     *  Si el Broker no esta corriendo aun, reintentamos en un bucle
     *  de espera pasiva (Sleep, no busy-waiting). */
    printf(ANSI_CLEAR);
    printf("\n  [MONITOR] Esperando que el Broker (M2) inicie...\n");
    fflush(stdout);

    int nIntentos = 0;
    const int MAX_INTENTOS = 60; /* Esperar hasta 60 segundos */

    while (g_bRunning && nIntentos < MAX_INTENTOS) {
        g_hMapFile = OpenFileMappingA(
            FILE_MAP_READ,       /* Solo lectura: el monitor no escribe en el buffer */
            FALSE,               /* No heredar el handle en procesos hijos */
            TCC_SHARED_MEM_NAME  /* Nombre definido en ipc_protocol.h */
        );

        if (g_hMapFile != NULL) break; /* Exito: el Broker ya creo el mapeo */

        nIntentos++;
        printf("  [MONITOR] Reintento %d/%d (Broker no disponible aun)...\r",
               nIntentos, MAX_INTENTOS);
        fflush(stdout);

        /* Espera pasiva de 1 segundo. WaitForSingleObject con un evento
         * dummy seria ideal, pero Sleep aqui es aceptable en una fase
         * de inicializacion de un solo intento por segundo. */
        Sleep(1000);
    }

    if (g_hMapFile == NULL) {
        fprintf(stderr, "\n  [MONITOR] Error: no se pudo abrir la memoria compartida "
                        "'%s' (error %lu).\n"
                        "  Asegurese de que el Broker (M2) este corriendo.\n",
                TCC_SHARED_MEM_NAME, GetLastError());
        Monitor_Limpiar();
        return MONITOR_ERR_MAP;
    }

    if (!g_bRunning) {
        /* El usuario cerro el monitor mientras esperaba al Broker */
        Monitor_Limpiar();
        return MONITOR_OK;
    }

    /* Mapear la vista de la memoria compartida en el espacio de este proceso.
     * MapViewOfFile devuelve un puntero al inicio de SHARED_BUFFER en memoria.
     * dwNumberOfBytesToMap = 0 significa "mapear todo el tamano del objeto". */
    g_pBuf = (const SHARED_BUFFER*)MapViewOfFile(
        g_hMapFile,
        FILE_MAP_READ, /* Solo lectura: cualquier escritura provocaria una excepcion */
        0,             /* Offset alto (32 bits): inicio */
        0,             /* Offset bajo (32 bits): inicio */
        0              /* Bytes a mapear: 0 = mapear todo el objeto */
    );

    if (g_pBuf == NULL) {
        fprintf(stderr, "  [MONITOR] Error: MapViewOfFile fallo (error %lu)\n",
                GetLastError());
        Monitor_Limpiar();
        return MONITOR_ERR_MAP;
    }

    printf("  [MONITOR] Memoria compartida abierta correctamente.\n");

    /* ── PASO 4: Abrir los eventos de coordinacion ─────────────
     *
     *  OpenEventA abre eventos EXISTENTES creados por el Broker (M2).
     *  EVENT_MODIFY_STATE: permiso minimo necesario para llamar SetEvent.
     *  No necesitamos EVENT_ALL_ACCESS; seguir el principio de minimo privilegio.
     *
     *  Si el Broker no creo los eventos aun (improbable si ya tiene el mapeo),
     *  lo reportamos como advertencia pero no abortamos: el dashboard
     *  seguira funcionando aunque los comandos D y S no tengan efecto. */
    g_hShutdownEvt = OpenEventA(
        EVENT_MODIFY_STATE, /* Solo necesitamos poder llamar SetEvent */
        FALSE,              /* No heredar en procesos hijos */
        TCC_EVT_SHUTDOWN_NAME
    );

    if (g_hShutdownEvt == NULL) {
        fprintf(stderr, "  [MONITOR] Advertencia: no se pudo abrir '%s' "
                        "(error %lu). El comando [S] no estara disponible.\n",
                TCC_EVT_SHUTDOWN_NAME, GetLastError());
    }

    g_hDebugEvt = OpenEventA(
        EVENT_MODIFY_STATE,
        FALSE,
        TCC_EVT_DEBUG_NAME
    );

    if (g_hDebugEvt == NULL) {
        fprintf(stderr, "  [MONITOR] Advertencia: no se pudo abrir '%s' "
                        "(error %lu). El comando [D] no estara disponible.\n",
                TCC_EVT_DEBUG_NAME, GetLastError());
    }

    printf("  [MONITOR] Eventos de control abiertos. Iniciando dashboard...\n");
    Sleep(800); /* Breve pausa para que el usuario lea el mensaje */

    /* Limpiar la pantalla antes del primer refresco */
    printf(ANSI_CLEAR);
    fflush(stdout);

    /* ── PASO 5: Bucle principal del monitor ────────────────────
     *
     *  El bucle tiene dos tareas:
     *    a. Esperar MONITOR_REFRESH_MS y refrescar el dashboard
     *    b. Verificar teclas presionadas con _kbhit() (no bloqueante)
     *
     *  Usamos WaitForSingleObject con timeout para la espera del refresco.
     *  Esto es ESPERA PASIVA del kernel: el proceso cede la CPU mientras
     *  espera. NO es busy-waiting. La CPU = ~0% durante el Sleep.
     *
     *  _kbhit() tampoco es busy-waiting: internamente consulta el buffer
     *  de entrada de la consola de Windows sin bloquear el hilo. */

    while (g_bRunning) {

        /* ── 5a. Renderizar el dashboard con datos actuales ──── */
        Monitor_DibujarDashboard(g_pBuf);

        /* ── 5b. Espera pasiva entre refrescos ──────────────────
         *  Dividimos el tiempo de espera en intervalos de 50 ms para
         *  poder responder a teclas con mas agilidad (max 50 ms de latencia)
         *  sin perder el ciclo de refresco de MONITOR_REFRESH_MS. */
        int msEsperados = 0;
        while (g_bRunning && msEsperados < MONITOR_REFRESH_MS) {
            Sleep(50);
            msEsperados += 50;

            /* ── 5c. Procesar entrada de teclado (no bloqueante) ─
             *
             *  _kbhit() devuelve != 0 si hay una tecla disponible en el
             *  buffer de entrada de la consola. No bloquea ni consume CPU.
             *  _getch() lee la tecla sin esperar y sin hacer echo. */
            if (_kbhit()) {
                int ch = _getch();

                /* Convertir a mayuscula para aceptar 'd', 'D', etc. */
                if (ch >= 'a' && ch <= 'z') ch -= 32;

                switch (ch) {

                case 'D': /* ── Comando: Toggle debug dump ──── */
                    if (g_hDebugEvt != NULL) {
                        /* SetEvent senaliza el evento al Broker.
                         * El Broker lo detecta en su bucle y alterna
                         * g_bDebugDump (luego hace ResetEvent). */
                        if (SetEvent(g_hDebugEvt)) {
                            printf("\n  %s[MONITOR] Debug dump toggle enviado al Broker.%s\n",
                                   ANSI_CYAN, ANSI_RESET);
                        } else {
                            printf("\n  %s[MONITOR] Error al enviar debug event (%lu)%s\n",
                                   ANSI_RED, GetLastError(), ANSI_RESET);
                        }
                        fflush(stdout);
                        Sleep(300); /* Pausa para que el usuario vea el mensaje */
                    } else {
                        printf("\n  %s[MONITOR] Evento de debug no disponible.%s\n",
                               ANSI_YELLOW, ANSI_RESET);
                        fflush(stdout);
                        Sleep(300);
                    }
                    break;

                case 'S': /* ── Comando: Apagar todo el sistema ── */
                    if (g_hShutdownEvt != NULL) {
                        printf("\n  %s[MONITOR] Enviando senal de apagado al sistema...%s\n",
                               ANSI_RED, ANSI_RESET);
                        fflush(stdout);

                        /* SetEvent en el shutdown event: el Broker lo detecta
                         * en su WaitForSingleObject del bucle principal y
                         * llama a Broker_Detener() de forma ordenada. */
                        if (!SetEvent(g_hShutdownEvt)) {
                            fprintf(stderr,
                                    "  [MONITOR] Error al enviar shutdown event (%lu)\n",
                                    GetLastError());
                        }

                        /* El monitor tambien se detiene tras enviar el shutdown */
                        Sleep(500);
                        g_bRunning = FALSE;
                    } else {
                        printf("\n  %s[MONITOR] Evento de shutdown no disponible.%s\n",
                               ANSI_YELLOW, ANSI_RESET);
                        fflush(stdout);
                        Sleep(300);
                    }
                    break;

                case 'Q': /* ── Comando: Cerrar solo el monitor ── */
                    printf("\n  %s[MONITOR] Cerrando monitor (el sistema sigue activo)...%s\n",
                           ANSI_YELLOW, ANSI_RESET);
                    fflush(stdout);
                    Sleep(500);
                    g_bRunning = FALSE;
                    break;

                default:
                    /* Tecla no reconocida: ignorar silenciosamente */
                    break;
                }
            }
        }
    }

    /* ── PASO 6: Limpieza ordenada de todos los recursos ────────
     *
     *  Se libera CADA handle adquirido durante la ejecucion.
     *  Obligatorio segun los requisitos del proyecto: ausencia total
     *  de fugas de handles en el kernel de Windows. */
    printf("\n  %s[MONITOR] Cerrando y liberando recursos...%s\n",
           ANSI_DIM ANSI_WHITE, ANSI_RESET);
    fflush(stdout);

    Monitor_Limpiar();

    printf("  %s[MONITOR] Monitor detenido limpiamente.%s\n\n",
           ANSI_GREEN, ANSI_RESET);

    return MONITOR_OK;
}
