/*#######################################################################################
 * Pontificia Universidad Javeriana
 * Fecha: 2026
 * Autores: Oscar Pinilla, David Pedraza, Johan Barreto
 * Programa: monitor.c
 * Descripción: Servidor central que gestiona la concurrencia del sistema. Utiliza un 
 *              hilo recolector para leer datos de un Pipe Nominal y múltiples hilos 
 *              consumidores para procesar la información mediante buffers circulares
 *              sincronizados con semáforos POSIX y Mutex.
 ######################################################################################*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <getopt.h>
#include "comun.h"

#define NUM_ESTACIONES 3
char *estaciones_nombres[] = {"EK", "ET", "EU"};

/* --- Estructuras de Sincronización --- */

// Representa un Buffer Circular para cada estación meteorológica
typedef struct {
    Medicion *datos;            // Almacenamiento dinámico de las medidas
    int inicio, fin, count;     // Punteros de lectura/escritura y contador de elementos
    int tamano, finalizado;     // Capacidad máxima y bandera de terminación
    sem_t vacios;               // Semáforo para controlar espacios disponibles
    sem_t llenos;               // Semáforo para controlar datos listos para consumir
    pthread_mutex_t mutex_buf;  // Mutex para garantizar acceso exclusivo al buffer
} BufferEstacion;

BufferEstacion buffers[NUM_ESTACIONES];
pthread_mutex_t mutex_archivo = PTHREAD_MUTEX_INITIALIZER; // Protege el archivo de salida
FILE *archivo_out;

// Variables globales para el cálculo de promedios (Sección Crítica)
double global_sum_h = 0, global_sum_r = 0, global_sum_p = 0;
int total_muestras = 0;

/* --- Lógica de Productor/Consumidor --- */

/**
 * Función Productora: Inserta una medición en el buffer circular.
 * Bloquea si el buffer está lleno utilizando semáforos.
 */
void producir(int idx, Medicion m) {
    sem_wait(&buffers[idx].vacios);           // Decrementa espacios vacíos
    pthread_mutex_lock(&buffers[idx].mutex_buf); // Entra a sección crítica del buffer

    buffers[idx].datos[buffers[idx].fin] = m;
    buffers[idx].fin = (buffers[idx].fin + 1) % buffers[idx].tamano;
    buffers[idx].count++;

    pthread_mutex_unlock(&buffers[idx].mutex_buf);
    sem_post(&buffers[idx].llenos);           // Incrementa contador de listos
}

/**
 * Función Consumidora: Extrae una medición del buffer circular.
 * Bloquea si no hay datos disponibles.
 */
Medicion consumir(int idx) {
    sem_wait(&buffers[idx].llenos);           // Espera a que existan datos
    pthread_mutex_lock(&buffers[idx].mutex_buf);

    Medicion m = buffers[idx].datos[buffers[idx].inicio];
    buffers[idx].inicio = (buffers[idx].inicio + 1) % buffers[idx].tamano;
    buffers[idx].count--;

    pthread_mutex_unlock(&buffers[idx].mutex_buf);
    sem_post(&buffers[idx].vacios);           // Libera un espacio en el buffer
    return m;
}

/* --- Hilos del Sistema --- */

/**
 * Hilo Recolector: Lee el flujo de datos del Pipe Nominal.
 * Distribuye las mediciones a los hilos consumidores según la estación.
 */
void *hilo_recolector(void *arg) {
    int fd = *(int *)arg;
    char buffer_raw[1024];
    ssize_t bytes;
    int terminados = 0;

    // Ciclo de lectura mientras el pipe esté activo y falten agentes por terminar
    while (terminados < NUM_ESTACIONES && (bytes = read(fd, buffer_raw, sizeof(buffer_raw)-1)) > 0) {
        buffer_raw[bytes] = '\0';
        char *linea = strtok(buffer_raw, "\n");

        while (linea) {
            // Detección de protocolo de fin enviado por un agenteM
            if (strncmp(linea, "FIN_", 4) == 0) {
                terminados++;
            } else {
                Medicion m;
                if (sscanf(linea, "%[^,],%d,%d,%d,%s", m.estacion, &m.humedad, &m.rocio, &m.presion, m.hora) == 5) {
                    for (int i=0; i<NUM_ESTACIONES; i++) {
                        if (strcmp(m.estacion, estaciones_nombres[i]) == 0) producir(i, m);
                    }
                }
            }
            linea = strtok(NULL, "\n");
        }
    }

    // Señalización de fin para despertar y cerrar hilos consumidores
    for(int i=0; i<NUM_ESTACIONES; i++) {
        buffers[i].finalizado = 1;
        sem_post(&buffers[i].llenos); 
    }
    return NULL;
}

/**
 * Hilo Consumidor: Procesa las mediciones de una estación específica.
 * Realiza el registro en disco y actualiza estadísticas globales.
 */
void *hilo_consumidor(void *arg) {
    int idx = *(int *)arg;
    while (1) {
        // Condición de salida: No hay más datos y el recolector terminó
        if (buffers[idx].finalizado && buffers[idx].count == 0) break;
        
        Medicion m = consumir(idx);
        
        // Verificación de seguridad para evitar procesar basura al cierre
        if (buffers[idx].finalizado && m.humedad == 0 && buffers[idx].count == 0) break;

        /* SECCIÓN CRÍTICA: Acceso compartido al archivo y variables globales */
        pthread_mutex_lock(&mutex_archivo);
        fprintf(archivo_out, "%s,%d,%d,%d,%s\n", m.estacion, m.humedad, m.rocio, m.presion, m.hora);
        global_sum_h += m.humedad; 
        global_sum_r += m.rocio; 
        global_sum_p += m.presion;
        total_muestras++;
        pthread_mutex_unlock(&mutex_archivo);
    }
    return NULL;
}

/* --- Orquestador Principal --- */

int main(int argc, char *argv[]) {
    int tamB = 0, opt;
    char *pNom = NULL;

    // Parsing de opciones: -b (tamaño buffer), -p (nombre pipe)
    while ((opt = getopt(argc, argv, "b:p:")) != -1) {
        if (opt == 'b') tamB = atoi(optarg);
        else if (opt == 'p') pNom = optarg;
    }

    if (!pNom || tamB <= 0) {
        fprintf(stderr, "Uso: ./monitor -b <tam_buffer> -p <nombre_pipe>\n");
        exit(EXIT_FAILURE);
    }

    // Creación del Pipe Nominal e inicialización del archivo consolidado
    mkfifo(pNom, 0666);
    archivo_out = fopen("consolidado.csv", "w");

    /* Inicialización de Buffers y Semáforos */
    for (int i=0; i<NUM_ESTACIONES; i++) {
        buffers[i].tamano = tamB;
        buffers[i].datos = malloc(sizeof(Medicion) * tamB);
        buffers[i].inicio = 0;
        buffers[i].fin = 0;
        buffers[i].count = 0;
        buffers[i].finalizado = 0;
        sem_init(&buffers[i].vacios, 0, tamB); // Comienza con todos los espacios libres
        sem_init(&buffers[i].llenos, 0, 0);    // Comienza sin datos
        pthread_mutex_init(&buffers[i].mutex_buf, NULL);
    }

    int fd = open(pNom, O_RDONLY); // Bloquea hasta que un agente abra para escritura
    pthread_t rec, cons[NUM_ESTACIONES];
    int ids[NUM_ESTACIONES];

    // Creación de hilos (Recolector + N Consumidores)
    pthread_create(&rec, NULL, hilo_recolector, &fd);
    for (int i=0; i<NUM_ESTACIONES; i++) {
        ids[i] = i;
        pthread_create(&cons[i], NULL, hilo_consumidor, &ids[i]);
    }

    // Barrera de sincronización: Esperar terminación de todos los hilos
    pthread_join(rec, NULL);
    for (int i=0; i<NUM_ESTACIONES; i++) pthread_join(cons[i], NULL);

    /* --- Reporte Meteorológico Final --- */
    if (total_muestras > 0) {
        double avgH = global_sum_h / total_muestras;
        double avgR = global_sum_r / total_muestras;
        double avgP = global_sum_p / total_muestras;

        printf("\nParte Meteorológico Bogotá: ");
        if (avgH > 90 && avgR > 9 && avgP < 750) printf("\"Lluvioso\"\n");
        else if (avgH >= 80 && avgH <= 95 && avgR > 8 && (int)avgP == 751) printf("\"Nublado\"\n");
        else if (avgH < 80 && avgR >= 5 && avgR <= 8 && avgP > 754) printf("\"Fresco\"\n");
        else printf("\"Variable\"\n");
    }

    printf("Monitor finalizado. Los datos consolidados están en 'consolidado.csv'.\n");
    fclose(archivo_out);
    return 0;
}