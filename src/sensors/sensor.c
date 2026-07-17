/*
 * TCC-System - Sistema de Telemetria y Control Concurrente
 * Modulo 1  - Subsistema de Sensores (Procesos Independientes)
 *
 * Desarrollador: Samuel Prado
 * C.I:          31.701.746
 *
 * sensor.c - Implementacion del proceso sensor independiente.
 *            Genera eventos de telemetria empaquetados y los
 *            transmite al broker central via Named Pipes en
 *            modo bloqueante. Soporta backpressure natural
 *            a traves del kernel de Windows.
 */

#define _WIN32_WINNT 0x0601
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sensor.h"


static volatile BOOL  g_bShutdown  = FALSE;
static       HANDLE  g_hPipe      = INVALID_HANDLE_VALUE;

static       LONGLONG g_llFreq    = 0;


static
ULONGLONG
GetTimestampUs(
    void
)
{
    LARGE_INTEGER li;
    if (g_llFreq == 0) {
        QueryPerformanceFrequency(&li);
        g_llFreq = li.QuadPart;
    }
    QueryPerformanceCounter(&li);
    return (ULONGLONG)((li.QuadPart * 1000000LL) / g_llFreq);
}


static
const char*
SensorTypeToStr(
    SENSOR_TYPE eType
)
{
    switch (eType) {
    case SENSOR_TYPE_ENGINE: return "Engine";
    case SENSOR_TYPE_TIRES:  return "Tires";
    case SENSOR_TYPE_BRAKES: return "Brakes";
    case SENSOR_TYPE_GPS:    return "GPS";
    default:                 return "Unknown";
    }
}


static
DWORD
SensorTypeToPayloadSize(
    SENSOR_TYPE eType
)
{
    switch (eType) {
    case SENSOR_TYPE_ENGINE: return TCC_SENSOR_PAYLOAD_ENGINE;
    case SENSOR_TYPE_TIRES:  return TCC_SENSOR_PAYLOAD_TIRES;
    case SENSOR_TYPE_BRAKES: return TCC_SENSOR_PAYLOAD_BRAKES;
    case SENSOR_TYPE_GPS:    return TCC_SENSOR_PAYLOAD_GPS;
    default:                 return TCC_MAX_PAYLOAD_SIZE;
    }
}


static
SENSOR_TYPE
StrToSensorType(
    const char* pszStr
)
{
    if (lstrcmpiA(pszStr, "engine") == 0) return SENSOR_TYPE_ENGINE;
    if (lstrcmpiA(pszStr, "tires")  == 0) return SENSOR_TYPE_TIRES;
    if (lstrcmpiA(pszStr, "brakes") == 0) return SENSOR_TYPE_BRAKES;
    if (lstrcmpiA(pszStr, "gps")    == 0) return SENSOR_TYPE_GPS;
    return 0;
}


BOOL WINAPI
CtrlHandler(
    DWORD dwCtrlType
)
{
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_bShutdown = TRUE;
        return TRUE;
    default:
        return FALSE;
    }
}


BOOL
Sensor_ParseArgs(
    int argc,
    char* argv[],
    SENSOR_CONFIG* pConfig
)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr,
            "Usage: sensor.exe <type> <id> [interval_ms]\n"
            "  type: engine | tires | brakes | gps\n"
            "  id:   unique sensor identifier (DWORD)\n"
            "  interval_ms: milliseconds between events (default: %u)\n",
            TCC_DEFAULT_INTERVAL_MS);
        return FALSE;
    }

    ZeroMemory(pConfig, sizeof(SENSOR_CONFIG));

    pConfig->eType = StrToSensorType(argv[1]);
    if (pConfig->eType == 0) {
        fprintf(stderr, "Error: unknown sensor type '%s'. Use: engine, tires, brakes, gps\n", argv[1]);
        return FALSE;
    }

    pConfig->dwId = (DWORD)atol(argv[2]);

    if (argc >= 4) {
        pConfig->dwIntervalMs = (DWORD)atol(argv[3]);
        if (pConfig->dwIntervalMs == 0) {
            pConfig->dwIntervalMs = TCC_DEFAULT_INTERVAL_MS;
        }
    } else {
        pConfig->dwIntervalMs = TCC_DEFAULT_INTERVAL_MS;
    }

    pConfig->dwPayloadSize = SensorTypeToPayloadSize(pConfig->eType);

    snprintf(pConfig->szPipePath, sizeof(pConfig->szPipePath),
             "%s%s_%lu", TCC_PIPE_PREFIX, SensorTypeToStr(pConfig->eType), pConfig->dwId);

    return TRUE;
}


BOOL
Sensor_ConnectPipe(
    SENSOR_CONFIG* pConfig,
    HANDLE* phPipe
)
{
    DWORD dwAttempt = 0;
    DWORD dwLastErr;

    printf("[SENSOR %s #%lu] Connecting to pipe: %s\n",
           SensorTypeToStr(pConfig->eType), pConfig->dwId, pConfig->szPipePath);

    while (!g_bShutdown && dwAttempt < TCC_PIPE_RETRY_COUNT) {
        *phPipe = CreateFileA(
            pConfig->szPipePath,
            GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (*phPipe != INVALID_HANDLE_VALUE) {
            printf("[SENSOR %s #%lu] Connected to pipe successfully.\n",
                   SensorTypeToStr(pConfig->eType), pConfig->dwId);
            g_hPipe = *phPipe;
            return TRUE;
        }

        dwLastErr = GetLastError();

        switch (dwLastErr) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PIPE_BUSY:
            dwAttempt++;
            printf("[SENSOR %s #%lu] Pipe not available (attempt %lu/%d), retrying in %ums...\n",
                   SensorTypeToStr(pConfig->eType), pConfig->dwId,
                   dwAttempt, TCC_PIPE_RETRY_COUNT, TCC_PIPE_WAIT_INTERVAL);
            Sleep(TCC_PIPE_WAIT_INTERVAL);
            break;

        default:
            fprintf(stderr, "[SENSOR %s #%lu] CreateFile failed: error %lu\n",
                    SensorTypeToStr(pConfig->eType), pConfig->dwId, dwLastErr);
            return FALSE;
        }
    }

    if (g_bShutdown) {
        printf("[SENSOR %s #%lu] Shutdown requested during pipe connection.\n",
               SensorTypeToStr(pConfig->eType), pConfig->dwId);
    } else {
        fprintf(stderr, "[SENSOR %s #%lu] Could not connect to pipe after %d attempts.\n",
                SensorTypeToStr(pConfig->eType), pConfig->dwId, TCC_PIPE_RETRY_COUNT);
    }

    return FALSE;
}


BOOL
Sensor_GeneratePayload(
    SENSOR_TYPE eType,
    BYTE* pBuffer,
    DWORD dwBufferSize,
    DWORD* pdwSize
)
{
    DWORD dwPayloadSize = SensorTypeToPayloadSize(eType);

    if (pBuffer == NULL || dwBufferSize < dwPayloadSize) {
        return FALSE;
    }

    for (DWORD i = 0; i < dwPayloadSize; i++) {
        pBuffer[i] = (BYTE)(rand() & 0xFF);
    }

    *pdwSize = dwPayloadSize;
    return TRUE;
}


BOOL
Sensor_GenerateAndSend(
    SENSOR_CONFIG* pConfig,
    HANDLE hPipe,
    SENSOR_STATS* pStats
)
{
    DWORD dwPayloadSize = pConfig->dwPayloadSize;
    DWORD dwTotalSize = SENSOR_EVENT_HEADER_SIZE + dwPayloadSize;

    BYTE* pBuffer = (BYTE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwTotalSize);
    if (pBuffer == NULL) {
        return FALSE;
    }

    SENSOR_EVENT_HEADER* pHeader = (SENSOR_EVENT_HEADER*)pBuffer;
    BYTE* pPayload = pBuffer + SENSOR_EVENT_HEADER_SIZE;

    pHeader->dwSensorId    = pConfig->dwId;
    pHeader->dwEventId     = pStats->dwEventsSent + 1;
    pHeader->ullTimestamp  = GetTimestampUs();
    pHeader->dwPayloadSize = dwPayloadSize;
    pHeader->bySensorType  = (BYTE)pConfig->eType;

    if (!Sensor_GeneratePayload(pConfig->eType, pPayload, dwPayloadSize, &dwPayloadSize)) {
        HeapFree(GetProcessHeap(), 0, pBuffer);
        return FALSE;
    }

    DWORD dwWritten = 0;
    if (!WriteFile(hPipe, pBuffer, dwTotalSize, &dwWritten, NULL)) {
        DWORD dwErr = GetLastError();
        if (dwErr == ERROR_BROKEN_PIPE || dwErr == ERROR_NO_DATA) {
            printf("[SENSOR %s #%lu] Pipe disconnected (broker closed).\n",
                   SensorTypeToStr(pConfig->eType), pConfig->dwId);
        } else {
            fprintf(stderr, "[SENSOR %s #%lu] WriteFile error: %lu\n",
                    SensorTypeToStr(pConfig->eType), pConfig->dwId, dwErr);
        }
        HeapFree(GetProcessHeap(), 0, pBuffer);
        pStats->dwErrors++;
        return FALSE;
    }

    if (dwWritten != dwTotalSize) {
        fprintf(stderr, "[SENSOR %s #%lu] Partial write: %lu/%lu bytes\n",
                SensorTypeToStr(pConfig->eType), pConfig->dwId,
                dwWritten, dwTotalSize);
        HeapFree(GetProcessHeap(), 0, pBuffer);
        pStats->dwErrors++;
        return FALSE;
    }

    pStats->dwEventsSent++;
    pStats->dwTotalBytes += dwWritten;

    HeapFree(GetProcessHeap(), 0, pBuffer);
    return TRUE;
}


int
main(
    int argc,
    char* argv[]
)
{
    SENSOR_CONFIG config;
    SENSOR_STATS  stats;
    HANDLE        hPipe = INVALID_HANDLE_VALUE;
    DWORD         dwEventCount = 0;
    int           exitCode = EXIT_SUCCESS;

    ZeroMemory(&stats, sizeof(stats));
    srand((unsigned int)(GetTickCount() ^ (DWORD)(UINT_PTR)argv));

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    if (!Sensor_ParseArgs(argc, argv, &config)) {
        return EXIT_FAILURE;
    }

    printf("\n");
    printf("============================================\n");
    printf("  TCC-System - Sensor Module\n");
    printf("  Type:     %s\n", SensorTypeToStr(config.eType));
    printf("  ID:       #%lu\n", config.dwId);
    printf("  Pipe:     %s\n", config.szPipePath);
    printf("  Interval: %lu ms\n", config.dwIntervalMs);
    printf("  Payload:  %lu bytes\n", config.dwPayloadSize);
    printf("============================================\n");
    printf("\n");

    if (!Sensor_ConnectPipe(&config, &hPipe)) {
        return EXIT_FAILURE;
    }

    printf("[SENSOR %s #%lu] Starting telemetry event loop...\n",
           SensorTypeToStr(config.eType), config.dwId);

    while (!g_bShutdown) {
        if (!Sensor_GenerateAndSend(&config, hPipe, &stats)) {
            exitCode = EXIT_FAILURE;
            break;
        }

        dwEventCount++;

        if ((dwEventCount % 100) == 0) {
            printf("[SENSOR %s #%lu] Sent %lu events, %lu bytes total\n",
                   SensorTypeToStr(config.eType), config.dwId,
                   stats.dwEventsSent, stats.dwTotalBytes);
        }

        if (config.dwIntervalMs > 0) {
            DWORD dwSleepStart = GetTickCount();
            while (!g_bShutdown) {
                DWORD dwElapsed = GetTickCount() - dwSleepStart;
                if (dwElapsed >= config.dwIntervalMs) break;
                Sleep(config.dwIntervalMs - dwElapsed);
            }
        }
    }

    printf("\n");
    printf("============================================\n");
    printf("  SENSOR %s #%lu SHUTDOWN\n", SensorTypeToStr(config.eType), config.dwId);
    printf("  Events sent:  %lu\n", stats.dwEventsSent);
    printf("  Bytes sent:   %lu\n", stats.dwTotalBytes);
    printf("  Errors:       %lu\n", stats.dwErrors);
    printf("============================================\n");

    if (hPipe != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(hPipe);
        CloseHandle(hPipe);
        g_hPipe = INVALID_HANDLE_VALUE;
        printf("[SENSOR %s #%lu] Pipe handle closed.\n",
               SensorTypeToStr(config.eType), config.dwId);
    }

    SetConsoleCtrlHandler(CtrlHandler, FALSE);

    return exitCode;
}
