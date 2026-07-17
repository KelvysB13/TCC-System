// Creador: Kelvys Concepcion
// Ultima Modificacion: 16/07/2026
// Descripcion: API del buffer circular compartido.

#pragma once
#include "../../include/common.h"

int  buf_iniciar(void);
int  buf_leer(Evento *ev);
int  buf_escribir(Evento *ev);
void buf_destruir(void);
