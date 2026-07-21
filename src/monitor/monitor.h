/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 * Modulo 4  - Monitor del Sistema y Dashboard de Control
 *
 * Desarrollador: Miguel Mora
 *
 * monitor.h - Cabecera del proceso Monitor independiente.
 *
 * PROPOSITO:
 *   Declara las constantes, tipos y funciones del monitor. Este modulo
 *   actua como la "consola de administracion" del sistema en tiempo real.
 *   Se ejecuta como proceso completamente independiente del Broker y del
 *   Dispatcher, y se comunica con ellos exclusivamente a traves de:
 *     - Memoria Compartida (File Mapping): lectura de estadisticas en vivo.
 *     - Eventos de Windows (CreateEvent): coordinacion del apagado y del
 *       toggle de debug dump en el Broker.
 *
 * COMANDOS DISPONIBLES EN EL DASHBOARD:
 *   [D] - Envia TCC_DebugEvent al Broker para activar/desactivar su
 *         volcado de depuracion por consola.
 *   [S] - Envia TCC_ShutdownEvent al Broker para iniciar el apagado
 *         controlado de todo el sistema.
 *   [Q] - Cierra el monitor de forma local y limpia sin afectar al Broker.
 *
 * ARQUITECTURA IPC (solo lectura/senalizacion, nunca escribe en el buffer):
 *   OpenFileMappingA  -> MapViewOfFile  -> SHARED_BUFFER* (solo lectura)
 *   OpenEventA (TCC_ShutdownEvent)     -> SetEvent para apagado del sistema
 *   OpenEventA (TCC_DebugEvent)        -> SetEvent para toggle de debug
 */

#ifndef MONITOR_H
#define MONITOR_H

/* Requerir al menos Windows 7 para CancelIoEx y otros APIs modernos */
#define _WIN32_WINNT 0x0601
/* Nota: _CRT_SECURE_NO_WARNINGS se define via linea de compilacion (/D) para
 * evitar redefinicion cuando se compila con MSVC (cl.exe). */

#include <windows.h>
#include "../../include/common.h"
#include "../../include/ipc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constantes del dashboard ─────────────────────────────────── */

/* Intervalo de refresco del dashboard en milisegundos.
 * 500 ms permite ver cambios en tiempo real sin saturar la consola. */
#define MONITOR_REFRESH_MS       500

/* Ancho de la barra de progreso del buffer circular (en caracteres) */
#define MONITOR_BAR_WIDTH        40

/* Nombre del proceso monitor para identificarse en el titulo de la consola */
#define MONITOR_WINDOW_TITLE     "TCC-System | Monitor M4 | Dashboard de Control"

/* ── Codigos de escape ANSI para colores en consola ──────────────
 *   Se activan mediante SetConsoleMode con ENABLE_VIRTUAL_TERMINAL_PROCESSING
 *   (disponible en Windows 10 Anniversary Update y superior). */
#define ANSI_RESET       "\x1b[0m"
#define ANSI_BOLD        "\x1b[1m"
#define ANSI_DIM         "\x1b[2m"
#define ANSI_RED         "\x1b[91m"
#define ANSI_GREEN       "\x1b[92m"
#define ANSI_YELLOW      "\x1b[93m"
#define ANSI_BLUE        "\x1b[94m"
#define ANSI_MAGENTA     "\x1b[95m"
#define ANSI_CYAN        "\x1b[96m"
#define ANSI_WHITE       "\x1b[97m"
#define ANSI_BG_BLACK    "\x1b[40m"
#define ANSI_BG_RED      "\x1b[41m"
#define ANSI_BG_GREEN    "\x1b[42m"

/* Mover cursor a posicion (1,1) y limpiar pantalla (solo para inicio) */
#define ANSI_CLEAR       "\x1b[2J\x1b[H"

/* ── Tipos de resultado de Monitor_Iniciar ────────────────────── */

/* Codigo de retorno del proceso monitor */
typedef enum {
    MONITOR_OK          = 0,   /* Ejecucion exitosa */
    MONITOR_ERR_MAP     = 1,   /* No pudo abrir la memoria compartida */
    MONITOR_ERR_EVENTS  = 2,   /* No pudo abrir los eventos de control */
    MONITOR_ERR_CONSOLE = 3    /* No pudo configurar la consola */
} MonitorResult;

/* ── Funciones publicas del monitor ──────────────────────────── */

/*
 * Monitor_HabilitarAnsi
 * Activa el procesamiento de codigos de escape ANSI en la consola de Windows.
 * Sin esto, los caracteres de escape se muestran en bruto en vez de con color.
 * Retorna TRUE si lo logro, FALSE si el terminal no lo soporta.
 */
BOOL Monitor_HabilitarAnsi(void);

/*
 * Monitor_DibujarDashboard
 * Renderiza el panel de control completo en la consola.
 * Recibe un puntero a la vista de solo lectura de la memoria compartida.
 * Llama a esta funcion periodicamente desde el bucle principal.
 */
void Monitor_DibujarDashboard(const SHARED_BUFFER* pBuf);

/*
 * Monitor_DibujarBarra
 * Dibuja una barra de progreso ASCII en la consola.
 *   valor:   valor actual
 *   maximo:  valor maximo posible
 *   ancho:   cantidad de caracteres de la barra
 */
void Monitor_DibujarBarra(LONG valor, LONG maximo, int ancho);

/*
 * Monitor_SensorTypeStr
 * Convierte el tipo de sensor numerico a cadena legible.
 * Centralizado aqui para no duplicar logica de otros modulos.
 */
const char* Monitor_SensorTypeStr(DWORD dwType);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_H */
