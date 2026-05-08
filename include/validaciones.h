/*#######################################################################################
 * Pontificia Universidad Javeriana
 * Fecha: 2026
 * Autores: Oscar Pinilla, David Pedraza, Johan Barreto
 * Programa: validaciones.h
 * Descripción: Definición de prototipos y firma de funciones para el módulo de 
 *              validación. Establece la interfaz necesaria para el filtrado de
 *              datos meteorológicos antes de su transmisión al Monitor.
 *######################################################################################*/

#ifndef VALIDACIONES_H
#define VALIDACIONES_H

#include "comun.h"

/**
 * @brief Valida si los datos capturados se encuentran dentro de los rangos lógicos.
 * 
 * Esta función es invocada por el Agente para decidir si una línea del archivo CSV
 * debe ser enviada a través del Pipe o si debe ser descartada por inconsistencia.
 * 
 * @param hum  Humedad relativa capturada.
 * @param roc  Punto de rocío capturado.
 * @param pres Presión atmosférica capturada.
 * @return int Retorna 1 (True) si la medida es válida, 0 (False) si es rechazada.
 */
int validarRangos(int hum, int roc, int pres);

#endif /* VALIDACIONES_H */