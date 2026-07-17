// Creador: Kelvys Concepcion
// Ultima Modificacion: 16/07/2026
// Descripcion: Punto de entrada, orquesta buffer y procesador.

#include "dispatcher.h"
#include "buffer.h"
#include "processor.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static volatile BOOL g_correr = TRUE;

// Manejador de Ctrl+C: detiene el bucle principal.
static BOOL WINAPI manejador_ctrl(DWORD dwCtrlType)
{
    (void)dwCtrlType;
    g_correr = FALSE;

    return TRUE;
}

// Punto de entrada: inicializa buffer, procesador y bucle principal.
int main(void)
{
    srand((unsigned)time(NULL));

    if (!SetConsoleCtrlHandler(manejador_ctrl, TRUE)) 
    {
        fprintf(stderr, "ERROR: SetConsoleCtrlHandler falló\n");
        return 1;
    }

    if (buf_iniciar() != 0) 
    {
        fprintf(stderr, "ERROR: buf_iniciar falló\n");
        return 1;
    }

    if (proc_iniciar() != 0) 
    {
        fprintf(stderr, "ERROR: proc_iniciar falló\n");
        buf_destruir();
        return 1;
    }

    printf("Dispatcher en ejecución (Ctrl+C para detener)...\n");
    fflush(stdout);

    while (g_correr) 
    {
        Evento ev;

        if (buf_leer(&ev) == 0) 
        {
            if (proc_enviar(&ev) != 0) 
            {
                fprintf(stderr, "ERROR: proc_enviar falló\n");
                break;
            }
        }
    }

    printf("\nApagando...\n");
    fflush(stdout);
    proc_apagar();
    buf_destruir();
    
    printf("Dispatcher detenido.\n");
    fflush(stdout);
    return 0;
}
