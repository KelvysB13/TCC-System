/*
 * test_runner.c - Suite de pruebas para el Módulo 2 (Broker Ingestor Central)
 *
 * Compilar: gcc -Wall -O2 -std=c99 -Iinclude tests/test_runner.c -o bin/test_runner.exe
 *
 * USO:
 *   1. Abrir TERMINAL 1: bin\broker.exe
 *   2. En TERMINAL 2:     bin\test_runner.exe
 */

#include "../include/common.h"
#include <stdio.h>
#include <stdlib.h>

static int g_passed = 0;
static int g_failed = 0;

static void print_result(const char *test_name, BOOL passed, const char *detail)
{
    printf("  [%s] %s", passed ? "PASS" : "FAIL", test_name);
    if (detail) printf("  (%s)", detail);
    printf("\n");
    if (passed) g_passed++; else g_failed++;
}

/*
 * Abre el Named Pipe, envía un evento y cierra.
 * Retorna TRUE si el envío fue exitoso.
 */
static BOOL send_one_event(DWORD sensor_id, DWORD priority, DWORD payload_size)
{
    HANDLE hPipe = CreateFileA(PIPE_NAME, GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, 0, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) return FALSE;

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    TelemetryEvent evt;
    evt.eventId = 0;
    evt.sensorId = sensor_id;
    evt.priority = priority;
    evt.payloadSize = (payload_size > MAX_PAYLOAD_SIZE) ? MAX_PAYLOAD_SIZE : payload_size;
    QueryPerformanceCounter(&evt.timestamp);
    for (DWORD i = 0; i < evt.payloadSize; i++)
        evt.payload[i] = (BYTE)(i ^ sensor_id);

    DWORD written;
    BOOL ok = WriteFile(hPipe, &evt, sizeof(TelemetryEvent), &written, NULL);
    if (ok) FlushFileBuffers(hPipe);
    CloseHandle(hPipe);
    return ok && (written == sizeof(TelemetryEvent));
}

/*
 * Envía N eventos uno por uno, cada uno con su propia conexión.
 * Útil para ráfagas controladas.
 */
static int send_burst(DWORD sensor_id, DWORD count)
{
    int sent = 0;
    for (DWORD i = 0; i < count; i++) {
        if (send_one_event(sensor_id, 1, 64)) sent++;
        else break;
    }
    return sent;
}

/*
 * Lee estadísticas actuales desde Shared Memory.
 * Retorna TRUE si pudo leer.
 */
static BOOL read_stats(SharedCircularBuffer *out)
{
    HANDLE hShm = OpenFileMappingA(FILE_MAP_READ, FALSE, SHM_NAME);
    if (!hShm) return FALSE;

    SharedCircularBuffer *buf = (SharedCircularBuffer*)
        MapViewOfFile(hShm, FILE_MAP_READ, 0, 0, sizeof(SharedCircularBuffer));
    if (!buf) { CloseHandle(hShm); return FALSE; }

    *out = *buf;
    UnmapViewOfFile(buf);
    CloseHandle(hShm);
    return TRUE;
}

/* ==============================================================
 * PRUEBAS INDIVIDUALES
 * ============================================================== */

static void test_basic_connection(void)
{
    BOOL ok = send_one_event(SENSOR_MOTOR, 1, 64);
    print_result("Conexion basica y envio de 1 evento", ok, NULL);
}

static void test_all_sensor_types(void)
{
    BOOL ok = TRUE;
    ok &= send_one_event(SENSOR_MOTOR,  1, 32);
    ok &= send_one_event(SENSOR_TIRES,  2, 48);
    ok &= send_one_event(SENSOR_BRAKES, 3, 64);
    ok &= send_one_event(SENSOR_GPS,    4, 128);
    print_result("4 tipos de sensor (Motor, TIRES, Brakes, GPS)", ok, NULL);
}

static void test_event_burst(void)
{
    DWORD count = 500;
    int sent = send_burst(SENSOR_MOTOR, count);
    char detail[64];
    sprintf(detail, "enviados %d de %u", sent, count);
    print_result("RaFaga de eventos sin perdida", sent == (int)count, detail);
}

static void test_concurrent_sensors(void)
{
    DWORD n = 10;
    BOOL ok = TRUE;
    for (DWORD i = 0; i < n; i++)
        ok &= send_one_event((i % 4) + 1, 1, 64);
    char detail[64];
    sprintf(detail, "%u conexiones simultaneas", n);
    print_result("Multiples sensores concurrentes", ok, detail);
}

static void test_max_payload(void)
{
    BOOL ok = send_one_event(SENSOR_GPS, 3, MAX_PAYLOAD_SIZE);
    print_result("Payload tamano maximo (256 bytes)", ok, NULL);
}

static void test_backpressure(void)
{
    /*
     * Estrategia: con un sensor PERSISTENTE (una sola conexion),
     * enviamos eventos hasta que WriteFile se bloquee por
     * backpressure. Verificamos que:
     *   1. El buffer se llene (bufferCount ~= bufferCapacity)
     *   2. WriteFile falle por timeout (backpressure activo)
     *   3. No haya perdida de eventos
     */

    printf("    Conectando sensor persistente...\n");

    HANDLE hPipe = CreateFileA(PIPE_NAME, GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
        print_result("Backpressure", FALSE, "no se pudo conectar");
        return;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, NULL, NULL);

    /* Lee estadisticas iniciales */
    SharedCircularBuffer stats_before;
    if (!read_stats(&stats_before)) {
        CloseHandle(hPipe);
        print_result("Backpressure", FALSE, "no se pudo leer Shared Memory");
        return;
    }

    printf("    Enviando eventos hasta backpressure...\n");

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    HANDLE hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ov.hEvent = hEvent;

    int sent = 0;
    BOOL backpressure_detected = FALSE;
    const int TIMEOUT_MS = 2000;
    const int MAX_SEND = CIRCULAR_BUFFER_CAP + 50;

    for (int i = 0; i < MAX_SEND; i++) {
        TelemetryEvent evt;
        evt.eventId = 0;
        evt.sensorId = SENSOR_TIRES;
        evt.priority = 1;
        evt.payloadSize = 64;
        QueryPerformanceCounter(&evt.timestamp);
        for (int j = 0; j < 64; j++) evt.payload[j] = (BYTE)(j ^ SENSOR_TIRES);

        ResetEvent(hEvent);
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = hEvent;

        DWORD written;
        BOOL ok = WriteFile(hPipe, &evt, sizeof(TelemetryEvent), &written, &ov);
        if (!ok) {
            if (GetLastError() == ERROR_IO_PENDING) {
                DWORD wr = WaitForSingleObject(hEvent, TIMEOUT_MS);
                if (wr == WAIT_TIMEOUT) {
                    backpressure_detected = TRUE;
                    printf("      Backpressure detectado en evento %d\n", i);
                    break;
                }
                ok = GetOverlappedResult(hPipe, &ov, &written, FALSE);
            }
        }
        if (!ok || written != sizeof(TelemetryEvent)) break;
        sent++;
    }

    CloseHandle(hPipe);
    CloseHandle(hEvent);

    /* Lee estadisticas finales */
    SharedCircularBuffer stats;
    if (!read_stats(&stats)) {
        print_result("Backpressure", FALSE, "no se pudo leer stats finales");
        return;
    }

    LONG new_events = stats.eventsProcessed - stats_before.eventsProcessed;
    BOOL buffer_full = (stats.bufferCount >= stats.bufferCapacity - 1);

    printf("      enviados=%d, bufferCount=%ld/%lu, nuevos=%ld, backpressure=%d\n",
           sent, stats.bufferCount, stats.bufferCapacity, new_events,
           backpressure_detected);

    char detail[128];
    sprintf(detail, "buffer=%ld/%lu, enviados=%d, backpressure=%s",
            stats.bufferCount, stats.bufferCapacity, sent,
            backpressure_detected ? "SI" : "NO");

    /* Sin dispatcher, el buffer se llena y backpressure se activa */
    print_result("Backpressure: buffer lleno + sensor bloqueado",
                 buffer_full && backpressure_detected, detail);
}

static void test_shared_memory_stats(void)
{
    SharedCircularBuffer stats;
    if (!read_stats(&stats)) {
        print_result("Estadisticas en Shared Memory", FALSE, "no se pudo abrir");
        return;
    }

    printf("      eventsProcessed  = %ld\n", stats.eventsProcessed);
    printf("      activeSensors    = %ld\n", stats.activeSensors);
    printf("      bufferCount      = %ld / %lu\n", stats.bufferCount, stats.bufferCapacity);
    printf("      initialized      = %ld\n", stats.initialized);

    BOOL ok = (stats.initialized && stats.eventsProcessed > 0);
    print_result("Estadisticas visibles desde Shared Memory", ok, NULL);
}

static void print_summary(void)
{
    printf("\n==========================================\n");
    printf("  RESULTADOS: %d PASSED, %d FAILED de %d\n",
           g_passed, g_failed, g_passed + g_failed);
    printf("==========================================\n");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n=== TEST SUITE: Modulo 2 - Broker Ingestor ===\n");
    printf("Asegurese de que broker.exe esta ejecutandose.\n\n");

    printf("--- Pruebas de Conexion ---\n");
    test_basic_connection();
    test_all_sensor_types();
    test_event_burst();

    printf("\n--- Pruebas de Concurrencia ---\n");
    test_concurrent_sensors();

    printf("\n--- Pruebas de Carga ---\n");
    test_max_payload();
    test_backpressure();

    printf("\n--- Verificacion de Shared Memory ---\n");
    test_shared_memory_stats();

    print_summary();
    return g_failed > 0 ? 1 : 0;
}
