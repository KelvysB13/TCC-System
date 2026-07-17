// Creador: Kelvys Concepcion
// Ultima Modificacion: 16/07/2026
// Descripcion: API del procesador de eventos.

#pragma once
#include "../../include/common.h"

int  proc_iniciar(void);
int  proc_enviar(Evento *ev);
void proc_apagar(void);
