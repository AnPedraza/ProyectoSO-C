/*#######################################################################################
 * Pontificia Universidad Javeriana
 * Fecha: 2026
 * Autores: Oscar Pinilla, David Pedraza, Johan Barreto
 * Programa: agenteM.c
 * Descripción: Agente de mediciones que actúa como proceso productor. Lee un archivo 
 *              CSV de sensores, aplica lógica de filtrado según rangos meteorológicos 
 *              y comunica los datos al proceso monitor a través de un Pipe Nominal (FIFO).
 ######################################################################################*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include "comun.h"        // Estructuras comunes del sistema
#include "validaciones.h" // Lógica de validación de rangos (humedad, rocío, presión)

int main(int argc, char *argv[]) {
    // --- Configuración de variables de entrada ---
    char *archivo = NULL, *pipeNom = NULL;
    int tiempo = 0, opt;

    /* 
     * Procesamiento de argumentos mediante getopt:
     * -f: Ruta del archivo CSV de entrada.
     * -t: Intervalo de tiempo (segundos) entre envíos.
     * -p: Nombre del pipe nominal para la comunicación IPC.
     */
    while ((opt = getopt(argc, argv, "f:t:p:")) != -1) {
        switch (opt) {
            case 'f': archivo = optarg; break;
            case 't': tiempo = atoi(optarg); break;
            case 'p': pipeNom = optarg; break;
            default:
                fprintf(stderr, "Uso: ./agenteM -f <file> -t <seg> -p <pipe>\n");
                exit(EXIT_FAILURE);
        }
    }

    // Validación de parámetros obligatorios
    if (!archivo || !pipeNom || tiempo <= 0) {
        fprintf(stderr, "Error: Argumentos insuficientes o inválidos.\n");
        fprintf(stderr, "Uso: ./agenteM -f <file> -t <seg> -p <pipe>\n");
        exit(EXIT_FAILURE);
    }

    // --- Apertura de Recursos ---
    
    // Apertura del archivo CSV en modo lectura
    FILE *fp = fopen(archivo, "r");
    if (!fp) { 
        perror("Error al abrir el archivo CSV"); 
        exit(EXIT_FAILURE); 
    }

    /* 
     * Apertura del Pipe Nominal (FIFO):
     * Se abre en modo O_WRONLY (Solo escritura). 
     * Bloqueará el proceso hasta que el Monitor lo abra en modo lectura.
     */
    int fdPipe = open(pipeNom, O_WRONLY);
    if (fdPipe < 0) { 
        perror("Error al abrir el Pipe Nominal"); 
        fclose(fp); 
        exit(EXIT_FAILURE); 
    }

    char linea[128], estacion[10], hora[12];
    int hum, roc, pres;

    printf("Agente [%s] iniciado. Transmitiendo datos cada %d seg...\n", archivo, tiempo);

    // --- Ciclo Principal de Lectura y Envío ---
    while (fgets(linea, sizeof(linea), fp)) {
        // Ignorar líneas que comiencen con punto o indicar fin de archivo manual
        if (linea[0] == '.') break;

        /* 
         * Extracción de campos del CSV:
         * Formato esperado: Estacion,Humedad,Rocio,Presion,Hora
         */
        if (sscanf(linea, "%[^,],%d,%d,%d,%s", estacion, &hum, &roc, &pres, hora) == 5) {
            
            /* 
             * Validación de Rangos:
             * Solo se envían al monitor las medidas que tengan sentido físico
             * según la lógica definida en validaciones.c.
             */
            if (validarRangos(hum, roc, pres)) {
                // Envío de la línea completa al Pipe (Comunicación IPC)
                write(fdPipe, linea, strlen(linea));
            } else {
                // Reporte de descarte por consola para auditoría
                printf("[SCADA - ALERTA] Datos descartados en %s (Fuera de rango): H:%d R:%d P:%d\n",
                        estacion, hum, roc, pres);
            }
        }
        
        /* 
         * Control de flujo:
         * El sleep permite simular la frecuencia de muestreo de un sensor real.
         */
        sleep(tiempo);
    }

    /* 
     * Protocolo de Finalización:
     * Se envía una cadena especial "FIN_NombreEstacion" para que el 
     * recolector del Monitor sepa que este agente ha terminado su tarea.
     */
    char finMsg[32];
    sprintf(finMsg, "FIN_%s\n", estacion);
    write(fdPipe, finMsg, strlen(finMsg));

    // --- Limpieza de Recursos ---
    printf("Agente [%s]: Transmisión finalizada con éxito.\n", archivo);
    close(fdPipe);
    fclose(fp);
    
    return 0;
}