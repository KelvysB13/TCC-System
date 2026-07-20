# TCC-System — Documentación Técnica
## Sistema de Telemetría y Control Concurrente

> **Universidad Nacional Experimental de Guayana**  
> Asignatura: Sistemas Operativos  
> Plataforma: Microsoft Windows — API Win32 nativa — Lenguaje C

---

## 👥 Equipo de Desarrollo

| Módulo | Responsable | C.I. |
|--------|-------------|------|
| 🚗 M1 — Subsistema de Sensores | Samuel Prado | 31.701.746 |
| 🧠 M2 — Broker Ingestor Central | Rolannys Sanchez | 28.550.912 |
| ⚙️ M3 — Dispatcher y Pool de Workers | Kelvys Concepción | — |
| 📊 M4 — Monitor del Sistema | Miguel Mora | — |

---

## 🏗️ Arquitectura General

El sistema simula el backend de una plataforma de telemetría automotriz (estilo Fórmula 1). La arquitectura está basada en un **pipeline completamente desacoplado**, donde cada módulo es un proceso independiente que se comunica exclusivamente a través de primitivas IPC del kernel de Windows.

```
┌─────────────┐   Named Pipe   ┌─────────────────────┐
│  M1 Sensor  │ ─────────────► │  M2 Broker (M-hilo) │
│  (N procs)  │                │  CreateThread por    │
└─────────────┘                │  sensor conectado    │
                               └──────────┬──────────┘
                                          │ File Mapping
                                          │ (SHARED_BUFFER)
                               ┌──────────▼──────────┐     ┌──────────────┐
                               │ M3 Dispatcher        │     │  M4 Monitor  │
                               │ Pool de Workers       │     │  (solo lect) │
                               │ LockFileEx → Log      │     │  dashboard   │
                               └─────────────────────┘     └──────────────┘
                                          ▲                        │
                                          └── Eventos Windows ─────┘
                                           TCC_ShutdownEvent
                                           TCC_DebugEvent
```

---

## 📦 Módulos del Sistema

---

### 🚗 Módulo 1 — Subsistema de Sensores
**Archivo:** `src/sensors/sensor.c`  
**Desarrollador:** Samuel Prado

Proceso independiente instanciable N veces en paralelo. Simula un sensor físico del vehículo (Motor, Neumáticos, Frenos, GPS).

**Funcionamiento:**
- Acepta por línea de comandos el **tipo** (`engine`/`tires`/`brakes`/`gps`) y el **ID** del sensor
- Genera eventos con `SENSOR_EVENT_HEADER` + payload aleatorio con timestamp de alta resolución (`QueryPerformanceCounter`)
- Se conecta al Broker mediante **Named Pipe** con reintentos automáticos (`WaitNamedPipeA`)
- Envía los eventos en modo bloqueante (`WriteFile`) — el backpressure del sistema lo frena naturalmente si el buffer se llena

**Primitivas Win32 clave:**
- `CreateFileA` — conexión al Named Pipe del Broker
- `WriteFile` — envío bloqueante de datos
- `QueryPerformanceCounter` / `QueryPerformanceFrequency` — timestamps de microsegundos
- `SetConsoleCtrlHandler` — manejo limpio de Ctrl+C

---

### 🧠 Módulo 2 — Broker Ingestor Central
**Archivo:** `src/broker/broker.c`  
**Desarrollador:** Rolannys Sanchez

Núcleo del sistema. Proceso multihilo que gestiona las conexiones de todos los sensores y centraliza los datos en memoria compartida.

**Funcionamiento:**
- Crea **8 Named Pipes** (2 instancias por tipo de sensor) y un hilo escuchador (`ListenerThread`) por cada uno
- Por cada sensor que se conecta (`ConnectNamedPipe`), crea dinámicamente un hilo lector dedicado (`ReaderThread`) mediante `CreateThread`
- El `ReaderThread` lee el evento, lo convierte a `struct Evento` y lo deposita en el **buffer circular** de la memoria compartida
- Registra la tabla de sensores activos en `SHARED_BUFFER.adwSensorIds[]` para que el Monitor la lea

**Sincronización (sin busy-waiting):**

| Objeto | Nombre en el kernel | Rol |
|--------|--------------------|----|
| `CreateSemaphoreA` | `TCC_BufEmpty` (init=64) | Slots libres en el buffer |
| `CreateSemaphoreA` | `TCC_BufFull` (init=0) | Slots con datos disponibles |
| `CreateMutexA` | `TCC_BufMutex` | Exclusión mutua sobre el buffer |
| `CreateEventA` | `TCC_ShutdownEvent` | Señal de apagado ordenado |
| `CreateEventA` | `TCC_DebugEvent` | Toggle de debug dump |

**Backpressure natural:** Si el buffer se llena, `WaitForSingleObject(TCC_BufEmpty)` bloquea al `ReaderThread`. Esto detiene la lectura del pipe. El pipe retiene datos. El sensor se bloquea en `WriteFile`. Sin bucles de espera activa.

**Primitivas Win32 clave:**
- `CreateFileMappingA` + `MapViewOfFile` — crear la memoria compartida
- `CreateNamedPipeA` — crear los pipes de escucha
- `ConnectNamedPipe` — espera bloqueante de conexión
- `CreateThread` — hilos por sensor
- `InterlockedIncrement` / `InterlockedDecrement` — estadísticas atómicas

---

### ⚙️ Módulo 3 — Dispatcher y Pool de Workers
**Archivos:** `src/dispatcher/dispatcher.c`, `buffer.c`, `processor.c`  
**Desarrollador:** Kelvys Concepción

Proceso consumidor que toma eventos del buffer compartido, los clasifica por prioridad y los distribuye a un pool de hilos trabajadores.

**Funcionamiento:**
- El `Dispatcher` lee eventos del buffer circular (vía semáforos y mutex nombrados)
- Clasifica cada evento en 4 colas de prioridad: `CRIT > ALTA > NORM > BAJA`
- Un pool de **4 Workers** (`CreateThread`) toma eventos de las colas y los procesa
- Cada worker persiste el resultado en `logs/dispatcher.log` usando `LockFileEx` para escritura concurrente segura

**Sincronización:**

| Objeto | Rol |
|--------|-----|
| `CRITICAL_SECTION` por cola | Exclusión mutua sobre cada cola de prioridad |
| `CreateSemaphoreW(g_hTrabajo)` | Señala a los workers que hay trabajo disponible |
| `CreateEventW(g_hParar)` | Señal de parada para los workers |
| `LockFileEx` | Bloqueo exclusivo para escritura en el log |

---

### 📊 Módulo 4 — Monitor del Sistema y Dashboard de Control
**Archivos:** `src/monitor/monitor.c`, `monitor.h`  
**Desarrollador:** Miguel Mora

Proceso completamente independiente que actúa como consola de administración del sistema en tiempo real.

**Funcionamiento:**
- Abre la memoria compartida del Broker en **modo solo lectura** (`OpenFileMappingA` + `MapViewOfFile` con `FILE_MAP_READ`)
- Lee periódicamente (cada 500 ms) las estadísticas de `SHARED_BUFFER` sin adquirir ningún mutex — los campos son `volatile LONG`, lo que garantiza lecturas atómicas en x86
- Renderiza un **dashboard de consola** con colores ANSI: barra de progreso del buffer, sensores activos, eventos procesados, tabla de sensores por tipo
- Si el Broker no está corriendo aún, reintenta con espera pasiva (`Sleep`) hasta 60 segundos

**Comandos interactivos (teclado no bloqueante):**

| Tecla | Efecto | Mecanismo |
|-------|--------|-----------|
| `D` | Toggle debug dump en el Broker | `SetEvent(TCC_DebugEvent)` |
| `S` | Apagar todo el sistema | `SetEvent(TCC_ShutdownEvent)` |
| `Q` | Cerrar solo el monitor | `g_bRunning = FALSE` |

**Primitivas Win32 clave:**
- `OpenFileMappingA(FILE_MAP_READ)` — abrir mapeo existente del Broker (solo lectura)
- `MapViewOfFile` — proyectar la memoria en el espacio del proceso
- `OpenEventA(EVENT_MODIFY_STATE)` — abrir eventos existentes para señalizarlos
- `SetEvent` — coordinar apagado y debug con el Broker
- `UnmapViewOfFile` + `CloseHandle` — limpieza completa de handles al salir

**El Monitor NUNCA escribe en el buffer circular ni adquiere el mutex del buffer.** Es un observador puro.

---

## 🔒 Propiedades de Corrección

### ✅ Zero Busy-Waiting
Toda sincronización usa bloqueo pasivo del kernel:
- `WaitForSingleObject(INFINITE)` — bloqueo total en semáforos y mutex
- `WaitForMultipleObjects` — espera múltiple sin polling
- `Sleep(ms)` — ceder CPU durante esperas de inicialización
- `_kbhit()` en el Monitor — consulta no bloqueante del buffer de teclado

### ✅ Ausencia de Race Conditions
- Escrituras en el buffer: protegidas por `TCC_BufMutex`
- Contadores globales: actualizados con `InterlockedIncrement/Decrement`
- Tabla de sensores: escritura serializada por el contexto del listener thread

### ✅ Prevención de Deadlocks
- Orden de adquisición consistente: siempre semáforo → luego mutex
- El Monitor nunca adquiere locks (solo lectura de volatiles)
- Todos los `WaitForSingleObject` tienen path de salida por shutdown event

### ✅ Cierre Limpio de Handles
Todos los procesos cierran sus handles al terminar:

| Recurso | Función de liberación |
|---------|----------------------|
| Vista de memoria compartida | `UnmapViewOfFile` |
| File Mapping | `CloseHandle(hMapFile)` |
| Semáforos, Mutex, Eventos | `CloseHandle` respectivo |
| Named Pipes | `DisconnectNamedPipe` + `CloseHandle` |
| Threads | `WaitForSingleObject` + `CloseHandle` |
| Critical Sections | `DeleteCriticalSection` |

---

## 🛠️ Compilación

### Con MinGW/GCC (Makefile):
```bash
make All
# Compila: bin/sensor.exe, bin/broker.exe, bin/dispatcher.exe, bin/monitor.exe
```

### Con MSVC (cl.exe):
```bash
cl.exe /W4 /O2 /D_WIN32_WINNT=0x0601 /D_CRT_SECURE_NO_WARNINGS /Iinclude src/sensors/sensor.c    /Fe:bin/sensor.exe     kernel32.lib
cl.exe /W4 /O2 /D_WIN32_WINNT=0x0601 /D_CRT_SECURE_NO_WARNINGS /Iinclude src/broker/broker.c     /Fe:bin/broker.exe     kernel32.lib
cl.exe /W4 /O2 /D_WIN32_WINNT=0x0601 /D_CRT_SECURE_NO_WARNINGS /Iinclude src/dispatcher/dispatcher.c src/dispatcher/buffer.c src/dispatcher/processor.c /Fe:bin/dispatcher.exe kernel32.lib
cl.exe /W4 /O2 /D_WIN32_WINNT=0x0601 /D_CRT_SECURE_NO_WARNINGS /Iinclude src/monitor/monitor.c   /Fe:bin/monitor.exe    kernel32.lib
```

---

## 🚀 Orden de Ejecución

```
1. bin\broker.exe          ← Primero (crea la memoria compartida y los pipes)
2. bin\dispatcher.exe      ← Segundo (se conecta a la memoria compartida)
3. bin\monitor.exe         ← Tercero (abre la memoria compartida en solo lectura)
4. bin\sensor.exe engine 1 ← Cuarto y siguientes (tantos como se desee)
   bin\sensor.exe tires 1
   bin\sensor.exe brakes 1
   bin\sensor.exe gps 1
   ... (hasta 8 simultáneos, 2 por tipo)
```

> [!IMPORTANT]
> El Broker debe estar activo antes que cualquier otro proceso, ya que es quien crea el File Mapping y los objetos de sincronización nombrados.

---

## 📁 Estructura del Repositorio

```
TCC-System/
├── include/
│   ├── common.h          # Tipos, estructuras y SHARED_BUFFER compartidos
│   └── ipc_protocol.h    # Nombres de objetos IPC del kernel (pipes, mapeos, eventos)
├── src/
│   ├── sensors/
│   │   ├── sensor.c      # M1 — Proceso sensor independiente
│   │   └── sensor.h
│   ├── broker/
│   │   ├── broker.c      # M2 — Broker multihilo
│   │   └── broker.h
│   ├── dispatcher/
│   │   ├── dispatcher.c  # M3 — Punto de entrada del dispatcher
│   │   ├── buffer.c      # M3 — Consumidor del buffer compartido
│   │   ├── buffer.h
│   │   ├── processor.c   # M3 — Pool de workers y log
│   │   └── processor.h
│   └── monitor/
│       ├── monitor.c     # M4 — Dashboard de control en tiempo real
│       └── monitor.h
├── bin/                  # Ejecutables compilados
├── docs/                 # Documentación y diagramas
├── Makefile              # Script de compilación (GCC/MinGW)
└── README.md
```
