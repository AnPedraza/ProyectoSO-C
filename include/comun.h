/*#######################################################################################
 * Pontificia Universidad Javeriana
 * Fecha: 2026
 * Autores: Oscar Pinilla, David Pedraza, Johan Barreto
 * Programa: comun.h
 * Descripción: Interfaz de datos compartida. Define el contrato de comunicación entre
 *              el Monitor y los Agentes, estableciendo los límites físicos de las
 *              mediciones y la estructura de datos que viaja por el Pipe Nominal.
 *######################################################################################*/

#ifndef COMUN_H
#define COMUN_H

/**
 * --- Parámetros Técnicos de Validación (Tabla 1) ---
 * Estas constantes definen los rangos operacionales permitidos para los sensores.
 * El uso de macros centralizadas facilita el mantenimiento del sistema si
 * los sensores son reemplazados o calibrados con nuevos rangos.
 */
#define MIN_HUM 77
#define MAX_HUM 100
#define MIN_PRES 740
#define MAX_PRES 760
#define MIN_ROCIO 3
#define MAX_ROCIO 12

/**
 * --- Estructura Medicion ---
 * Representa la unidad mínima de información del sistema.
 * 
 * @field estacion: Identificador de la estación de origen (ej. "EK", "ET", "EU").
 * @field humedad:  Porcentaje de humedad relativa registrado.
 * @field rocio:    Temperatura del punto de rocío en grados Celsius.
 * @field presion:  Presión atmosférica medida en hPa.
 * @field hora:     Marca de tiempo del registro (Formato HH:MM:SS).
 */
typedef struct {
    char estacion[4];   // Espacio para 3 caracteres + terminador nulo
    int humedad;
    int rocio;
    int presion;
    char hora[10];      // Formato esperado: HH:MM:SS
} Medicion;

#endif /* COMUN_H */