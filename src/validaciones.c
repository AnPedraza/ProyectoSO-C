/*#######################################################################################
 * Pontificia Universidad Javeriana
 * Fecha: 2026
 * Autores: Oscar Pinilla, David Pedraza, Johan Barreto
 * Programa: validaciones.c
 * Descripción: Módulo de validación de integridad de datos. Implementa la lógica de 
 *              filtrado para las variables atmosféricas (humedad, presión y punto de rocío) 
 *              según los parámetros técnicos establecidos en la Tabla 1 del proyecto.
 *######################################################################################*/

#include "validaciones.h"
#include <stdio.h>

/**
 * validarRangos: Evalúa si una medición cumple con los límites físicos permitidos.
 * 
 * Esta función actúa como un predicado de control. Si alguna de las variables
 * se encuentra fuera de los límites definidos en 'validaciones.h', la medición
 * se considera ruido o error de sensor y es descartada para no contaminar el 
 * cálculo de promedios en el Monitor.
 * 
 * @param hum  Valor de humedad relativa (%)
 * @param roc  Valor del punto de rocío (°C)
 * @param pres Valor de presión atmosférica (hPa)
 * @return int 1 si la medición es íntegra y válida; 0 en caso contrario.
 */
int validarRangos(int hum, int roc, int pres) {
    
    // --- Validación de Humedad ---
    if (hum < MIN_HUM || hum > MAX_HUM) {
        // La humedad debe estar dentro del rango operacional del sensor
        return 0; 
    }
    
    // --- Validación de Punto de Rocío ---
    if (roc < MIN_ROCIO || roc > MAX_ROCIO) {
        // Filtra valores térmicos inconsistentes
        return 0; 
    }
    
    // --- Validación de Presión ---
    if (pres < MIN_PRES || pres > MAX_PRES) {
        // Asegura que la presión corresponda a rangos barométricos estándar
        return 0; 
    }

    /* 
     * Si todas las pruebas anteriores pasan, los datos se consideran confiables
     * para ser transmitidos a través del Pipe Nominal al proceso recolector.
     */
    return 1;
}