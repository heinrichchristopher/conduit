/*
 * Anpassungen an der lwIP-Konfiguration von nxdk (lib/net/nforceif/include/lwipopts.h).
 * Wird per -include vor jeder Quelldatei von lwIP und von Conduit eingebunden (siehe Makefile).
 * lwipopts.h setzt diese Werte ohne #ifndef, ein -D auf der Kommandozeile wuerde daher ueberschrieben.
 * Deshalb hier zuerst lwipopts.h laden (dessen Include-Waechter verhindert ein zweites Laden) und danach aendern.
 */
#pragma once
#include "lwipopts.h"

/* nxdk baut alle Debug-Ausgaben von lwIP ein (zur Laufzeit abgeschaltet): kostet Platz und je Paket Abfragen */
#undef LWIP_DEBUG
