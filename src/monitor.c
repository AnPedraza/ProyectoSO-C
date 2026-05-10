/*#######################################################################################
 * Pontificia Universidad Javeriana
 * Fecha: 2026
 * Autores: Oscar Pinilla, David Pedraza, Johan Barreto
 * Programa: monitor.c
 *
 * Descripción:
 *   Servidor central (Control de Categorización) del Sistema Meteorológico.
 *   Su arquitectura interna sigue el patrón PRODUCTOR/CONSUMIDOR con buffer acotado:
 *
 *     [Pipe Nominal] --> [Hilo Recolector / Productor] --> [Buffer Circular EK]
 *                                                       --> [Buffer Circular ET]
 *                                                       --> [Buffer Circular EU]
 *
 *   Cada buffer es atendido por un Hilo Consumidor dedicado que escribe al
 *   archivo consolidado.csv y acumula promedios globales para la categorización
 *   meteorológica final.
 *
 * Sincronización empleada:
 *   - Semáforos POSIX (sem_t):
 *       * vacios: cuenta espacios disponibles en el buffer (productor espera aquí)
 *       * llenos: cuenta datos listos para consumir (consumidor espera aquí)
 *   - Mutex POSIX (pthread_mutex_t):
 *       * mutex_buf: protege índices inicio/fin/count de cada buffer individual
 *       * mutex_archivo: protege la escritura en consolidado.csv y los acumuladores globales
 *
 * Invocación:
 *   ./monitor -b <tam_buffer> -p <nombre_pipe>
 *   Ejemplo: ./monitor -b 4 -p pipeNom
 ######################################################################################*/

/* =====================================================================================
 * INCLUDES
 * Bibliotecas estándar de C y POSIX necesarias para el programa.
 * ===================================================================================== */
#include <stdio.h>       // printf, fprintf, fopen, fclose, fflush
#include <stdlib.h>      // malloc, free, exit, atoi
#include <string.h>      // strcmp, strncmp, strtok, sscanf
#include <unistd.h>      // read, close, unlink, sleep
#include <fcntl.h>       // open, O_RDONLY
#include <pthread.h>     // pthread_create, pthread_join, pthread_mutex_t
#include <semaphore.h>   // sem_t, sem_init, sem_wait, sem_post, sem_timedwait
#include <sys/stat.h>    // mkfifo
#include <getopt.h>      // getopt para parseo de argumentos
#include <time.h>        // clock_gettime, struct timespec (requerido por sem_timedwait)
#include "../include/comun.h"       // Definición de Medicion y constantes de rango
#include "../include/validaciones.h"
/* =====================================================================================
 * CONSTANTES Y DATOS GLOBALES DE CONFIGURACIÓN
 * ===================================================================================== */

/*
 * NUM_ESTACIONES: cantidad fija de estaciones IDEAM soportadas por el sistema.
 * Cambiar este valor requiere también actualizar estaciones_nombres[].
 */
#define NUM_ESTACIONES 3

/*
 * Nombres de las estaciones en el mismo orden en que se crearán los buffers.
 * El índice en este arreglo corresponde al índice en el arreglo buffers[].
 *   buffers[0] → "EK" (Kennedy)
 *   buffers[1] → "ET" (Teusaquillo)
 *   buffers[2] → "EU" (Usaquén)
 */
char *estaciones_nombres[] = {"EK", "ET", "EU"};

/* =====================================================================================
 * ESTRUCTURA DEL BUFFER CIRCULAR (uno por estación)
 *
 * Un buffer circular acotado (bounded buffer) es la estructura clásica del
 * patrón Productor/Consumidor. Tiene capacidad fija (tamano), y usa dos
 * punteros circulares (inicio y fin) para gestionar inserciones y extracciones
 * sin mover datos en memoria.
 *
 * Invariante: count == (fin - inicio + tamano) % tamano
 * ===================================================================================== */
typedef struct {
    Medicion *datos;            // Arreglo dinámico de mediciones (capacidad = tamano)
    int inicio;                 // Índice de la próxima lectura (consumidor)
    int fin;                    // Índice de la próxima escritura (productor)
    int count;                  // Número de elementos actuales en el buffer
    int tamano;                 // Capacidad máxima (argumento -b del usuario)

    /*
     * finalizado: bandera atómica (int) que el Hilo Recolector activa al terminar.
     * Cuando finalizado=1 y count=0, los consumidores saben que no habrá más datos
     * y pueden salir de su bucle de forma segura.
     */
    int finalizado;

    /*
     * vacios: semáforo que cuenta los ESPACIOS LIBRES disponibles en el buffer.
     *   - Inicializado en tamano (buffer completamente vacío al inicio).
     *   - El PRODUCTOR hace sem_wait(vacios) antes de insertar: si vale 0, se bloquea.
     *   - El CONSUMIDOR hace sem_post(vacios) después de extraer: libera un espacio.
     */
    sem_t vacios;

    /*
     * llenos: semáforo que cuenta los ELEMENTOS DISPONIBLES para consumir.
     *   - Inicializado en 0 (buffer vacío al inicio).
     *   - El CONSUMIDOR hace sem_wait(llenos) antes de extraer: si vale 0, se bloquea.
     *   - El PRODUCTOR hace sem_post(llenos) después de insertar: avisa que hay dato.
     */
    sem_t llenos;

    /*
     * mutex_buf: mutex que protege la SECCIÓN CRÍTICA del buffer.
     * Garantiza que productor y consumidor no modifiquen inicio/fin/count al mismo tiempo.
     * Solo se toma DESPUÉS de pasar el semáforo (evita deadlock).
     */
    pthread_mutex_t mutex_buf;

} BufferEstacion;

/* =====================================================================================
 * VARIABLES GLOBALES DEL SISTEMA
 * ===================================================================================== */

/* Un buffer por estación */
BufferEstacion buffers[NUM_ESTACIONES];

/*
 * mutex_archivo: protege el acceso concurrente al archivo consolidado.csv
 * y a los acumuladores globales de promedios.
 * Sin este mutex, dos hilos consumidores podrían escribir simultáneamente
 * y corromper el archivo o los valores estadísticos.
 */
pthread_mutex_t mutex_archivo = PTHREAD_MUTEX_INITIALIZER;

/* Puntero al archivo de salida consolidado */
FILE *archivo_out;

/*
 * Acumuladores globales para el cálculo del promedio final.
 * Son SECCIÓN CRÍTICA: solo se acceden dentro del bloque protegido por mutex_archivo.
 */
double global_sum_h = 0;   // Suma acumulada de humedad
double global_sum_r = 0;   // Suma acumulada de rocío
double global_sum_p = 0;   // Suma acumulada de presión
int total_muestras = 0;    // Contador total de mediciones procesadas

/* =====================================================================================
 * FUNCIÓN: producir
 *
 * Inserta una medición en el buffer circular de la estación con índice 'idx'.
 * Implementa la lógica del PRODUCTOR en el patrón clásico:
 *
 *   1. sem_wait(vacios)  → espera hasta que haya al menos un espacio libre
 *   2. lock(mutex_buf)   → inicia sección crítica del buffer
 *   3. inserción         → escribe dato en buffers[idx].datos[fin] y avanza fin
 *   4. unlock(mutex_buf) → termina sección crítica
 *   5. sem_post(llenos)  → avisa al consumidor que hay un nuevo elemento
 *
 * IMPORTANTE: El orden sem_wait → lock es obligatorio. Si fuera al revés
 * (lock → sem_wait), el consumidor no podría tomar el mutex mientras espera,
 * generando un DEADLOCK.
 *
 * @param idx  Índice del buffer destino (0=EK, 1=ET, 2=EU)
 * @param m    Estructura Medicion con los datos a almacenar
 * ===================================================================================== */
void producir(int idx, Medicion m) {
    /*
     * PASO 1: Esperar espacio disponible.
     * Si el buffer está lleno (vacios == 0), este hilo se bloquea aquí
     * hasta que un consumidor extraiga un elemento y haga sem_post(vacios).
     */
    sem_wait(&buffers[idx].vacios);

    /*
     * PASO 2: Tomar el mutex del buffer.
     * Garantiza acceso exclusivo a los punteros inicio/fin/count.
     */
    pthread_mutex_lock(&buffers[idx].mutex_buf);

    /* PASO 3: Insertar el dato en la posición 'fin' del arreglo circular */
    buffers[idx].datos[buffers[idx].fin] = m;

    /*
     * Avanzar 'fin' de forma circular: cuando llega al final del arreglo,
     * vuelve al inicio (módulo del tamaño = comportamiento circular).
     */
    buffers[idx].fin = (buffers[idx].fin + 1) % buffers[idx].tamano;
    buffers[idx].count++; // Registrar que hay un elemento más en el buffer

    /* PASO 4: Liberar el mutex */
    pthread_mutex_unlock(&buffers[idx].mutex_buf);

    /*
     * PASO 5: Señalizar al consumidor.
     * Incrementa 'llenos' para indicar que hay un dato listo para procesar.
     * Si el consumidor estaba bloqueado en sem_wait(llenos), se despierta.
     */
    sem_post(&buffers[idx].llenos);
}

/* =====================================================================================
 * FUNCIÓN: consumir
 *
 * Extrae una medición del buffer circular de la estación con índice 'idx'.
 * Implementa la lógica del CONSUMIDOR (simétrica e inversa al productor):
 *
 *   1. sem_wait(llenos)  → espera hasta que haya al menos un dato disponible
 *   2. lock(mutex_buf)   → inicia sección crítica del buffer
 *   3. extracción        → lee dato de buffers[idx].datos[inicio] y avanza inicio
 *   4. unlock(mutex_buf) → termina sección crítica
 *   5. sem_post(vacios)  → avisa al productor que se liberó un espacio
 *
 * NOTA: Esta versión clásica se usa internamente. El hilo consumidor usa
 * sem_timedwait en su bucle principal para evitar bloqueo eterno al finalizar.
 *
 * @param idx  Índice del buffer fuente
 * @return     Estructura Medicion extraída del buffer
 * ===================================================================================== */
Medicion consumir(int idx) {
    /* PASO 1: Esperar dato disponible */
    sem_wait(&buffers[idx].llenos);

    /* PASO 2: Tomar mutex del buffer */
    pthread_mutex_lock(&buffers[idx].mutex_buf);

    /* PASO 3: Extraer el dato de la posición 'inicio' */
    Medicion m = buffers[idx].datos[buffers[idx].inicio];

    /* Avanzar 'inicio' circularmente */
    buffers[idx].inicio = (buffers[idx].inicio + 1) % buffers[idx].tamano;
    buffers[idx].count--; // Un elemento menos en el buffer

    /* PASO 4: Liberar mutex */
    pthread_mutex_unlock(&buffers[idx].mutex_buf);

    /* PASO 5: Liberar un espacio para el productor */
    sem_post(&buffers[idx].vacios);

    return m;
}

/* =====================================================================================
 * HILO: hilo_recolector  (PRODUCTOR principal del sistema)
 *
 * Responsabilidades:
 *   1. Leer bytes crudos del Pipe Nominal (FIFO) de forma continua.
 *   2. Tokenizar las líneas recibidas (separadas por '\n').
 *   3. Detectar señales de finalización "FIN_*" enviadas por los agentes.
 *   4. Parsear líneas de datos CSV y clasificarlas al buffer de la estación correcta.
 *   5. Al recibir N señales FIN_ (una por agente), activar las banderas de cierre
 *      y despertar a todos los consumidores que puedan estar bloqueados.
 *
 * [BUG 2 CORREGIDO]:
 *   La versión anterior usaba 'goto fin_lectura' al detectar el PRIMER FIN_,
 *   ignorando los datos de los demás agentes. Ahora se cuenta cada FIN_
 *   y solo se cierra cuando se reciben NUM_ESTACIONES señales.
 *
 * @param arg  Puntero al descriptor de archivo (int*) del pipe abierto
 * @return     NULL (convención de pthreads)
 * ===================================================================================== */
void *hilo_recolector(void *arg) {
    int fd = *(int *)arg;
    char buffer_raw[4096]; // Buffer de lectura cruda del pipe (aumentado para ráfagas)
    ssize_t bytes;

    /*
     * [BUG 2 - CORRECCIÓN]:
     * Contador de agentes que han enviado su señal FIN_.
     * Solo cuando terminados == NUM_ESTACIONES se cierra el pipe.
     */
    int terminados = 0;

    printf("[RECOLECTOR] Iniciado. Escuchando en el Pipe Nominal...\n");
    fflush(stdout);

    /*
     * Bucle principal de lectura del pipe.
     * read() es bloqueante: si no hay datos en el pipe, el hilo duerme aquí
     * hasta que algún agente escriba. Retorna 0 cuando todos los escritores
     * cierran su extremo del pipe (todos los agentes terminaron).
     */
    while ((bytes = read(fd, buffer_raw, sizeof(buffer_raw) - 1)) > 0) {

        /* Asegurar que el buffer sea una cadena C válida */
        buffer_raw[bytes] = '\0';

        /*
         * Tokenización por saltos de línea.
         * Los agentes pueden enviar múltiples líneas en una sola llamada a write(),
         * por lo que debemos procesar cada línea individualmente.
         * strtok modifica buffer_raw al reemplazar '\n' con '\0'.
         */
        char *linea = strtok(buffer_raw, "\n");

        while (linea != NULL) {

            /*
             * 
             * Protocolo de finalización: cada agente envía "FIN_<estacion>" al terminar.
             * En lugar de salir al primer FIN_, incrementamos el contador.
             * Solo cuando todos los agentes han finalizado (terminados == NUM_ESTACIONES)
             * salimos del bucle principal.
             */
            if (strncmp(linea, "FIN_", 4) == 0) {
                terminados++;
                printf("[RECOLECTOR] Agente finalizado: %s (%d/%d agentes terminados)\n",
                       linea, terminados, NUM_ESTACIONES);
                fflush(stdout);

                /* ¿Terminaron TODOS los agentes? Entonces cerramos */
                if (terminados >= NUM_ESTACIONES) {
                    goto fin_lectura;
                }

            } else {
                /*
                 * Línea de datos: intentar parsear formato CSV.
                 * Formato esperado: ESTACION,HUMEDAD,ROCIO,PRESION,HH:MM:SS
                 * Ejemplo:          EK,90,9,750,08:00:00
                 */
                Medicion m;
                if (sscanf(linea, "%[^,],%d,%d,%d,%s",
                           m.estacion, &m.humedad, &m.rocio, &m.presion, m.hora) == 5) {

                    printf("[RECOLECTOR] Recibido de %s | Hum: %d%% | Rocío: %d°C | Pres: %d hPa\n",
                           m.estacion, m.humedad, m.rocio, m.presion);
                    fflush(stdout);

                    /*
                     * Clasificación: buscar a qué buffer pertenece esta medición
                     * comparando el campo m.estacion con los nombres registrados.
                     * Se produce al buffer correspondiente.
                     */
                    for (int i = 0; i < NUM_ESTACIONES; i++) {
                        if (strcmp(m.estacion, estaciones_nombres[i]) == 0) {
                            producir(i, m); // ← Puede bloquear si el buffer está lleno
                            break;
                        }
                    }
                }
                /* Si sscanf != 5, la línea está malformada; la ignoramos silenciosamente */
            }

            /* Continuar con la siguiente línea del mismo bloque leído */
            linea = strtok(NULL, "\n");
        }
    }

fin_lectura:
    /*
     * Al llegar aquí (todos los agentes finalizaron o pipe cerrado):
     * Activar banderas de finalización para TODOS los buffers
     */
    printf("[RECOLECTOR] Todos los agentes finalizaron. Cerrando buffers...\n");
    fflush(stdout);

    for (int i = 0; i < NUM_ESTACIONES; i++) {
        buffers[i].finalizado = 1;
        /*
         * Múltiples sem_post para despertar al consumidor que podría estar
         * bloqueado en sem_wait(llenos) después de haber procesado el último dato.
         */
        sem_post(&buffers[i].llenos);
        sem_post(&buffers[i].llenos);
    }

    return NULL;
}

/* =====================================================================================
 * HILO: hilo_consumidor  (CONSUMIDOR dedicado por estación)
 *
 * Cada instancia de este hilo atiende exclusivamente UN buffer (una estación).
 * Se crean NUM_ESTACIONES instancias, cada una recibiendo su índice como argumento.
 *
 * Responsabilidades:
 *   1. Extraer mediciones del buffer circular de su estación.
 *   2. Escribir cada medición en el archivo consolidado.csv (sección crítica global).
 *   3. Acumular valores para el cálculo de promedios estadísticos.
 *   4. Terminar cuando el recolector haya finalizado Y el buffer esté vacío.
 *
 * @param arg  Puntero a int con el índice del buffer a atender
 * @return     NULL
 * ===================================================================================== */
void *hilo_consumidor(void *arg) {
    int idx = *(int *)arg;

    printf("[CONSUMIDOR-%s] Iniciado, atendiendo buffer %d.\n",
           estaciones_nombres[idx], idx);
    fflush(stdout);

    while (1) {
        /*
         * CONDICIÓN DE SALIDA (verificar ANTES de intentar consumir):
         * Si el recolector ya terminó (finalizado=1) y no quedan datos (count=0),
         * no hay nada más que procesar: terminar el hilo.
         */
        if (buffers[idx].finalizado && buffers[idx].count == 0) {
            break;
        }

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1; // Timeout: 1 segundo desde ahora

        if (sem_timedwait(&buffers[idx].llenos, &ts) == -1) {
            /*
             * Timeout expirado: no había datos en 1 segundo.
             * Volver al inicio del while para re-chequear condición de salida.
             */
            continue;
        }

        /*
         * sem_timedwait tuvo éxito: hay al menos 1 dato disponible.
         * Tomar el mutex para acceso exclusivo al buffer.
         */
        pthread_mutex_lock(&buffers[idx].mutex_buf);

        /*
         * Doble verificación: aunque el semáforo indicó que hay datos,
         * verificamos count > 0 por robustez (podría haber race condition
         * en casos de diseño más complejos).
         */
        if (buffers[idx].count == 0) {
            pthread_mutex_unlock(&buffers[idx].mutex_buf);
            /*
             * Si finalizado y vacío → salir. Si no, el sem_post extra del
             * recolector causó este caso falso; volver al inicio del while.
             */
            if (buffers[idx].finalizado) break;
            continue;
        }

        /* Extraer el dato del buffer circular */
        Medicion m = buffers[idx].datos[buffers[idx].inicio];
        buffers[idx].inicio = (buffers[idx].inicio + 1) % buffers[idx].tamano;
        buffers[idx].count--;

        pthread_mutex_unlock(&buffers[idx].mutex_buf);

        /* Señalizar al productor que hay un espacio libre */
        sem_post(&buffers[idx].vacios);

        /* -----------------------------------------------------------------------
         * SECCIÓN CRÍTICA GLOBAL: escritura en archivo y acumuladores
         *
         * Solo UN hilo consumidor a la vez puede entrar aquí, protegido por
         * mutex_archivo. Sin esto, dos hilos podrían intercalar escrituras
         * en el archivo y corromper el CSV, o producir race conditions en
         * las variables global_sum_*.
         * ----------------------------------------------------------------------- */
        pthread_mutex_lock(&mutex_archivo);

        /* Escribir la medición en el archivo consolidado en formato CSV */
        fprintf(archivo_out, "%s,%d,%d,%d,%s\n",
                m.estacion, m.humedad, m.rocio, m.presion, m.hora);
        fflush(archivo_out); // Persistencia inmediata (evita pérdida si el proceso falla)

        /* Acumular para promedios finales */
        global_sum_h += m.humedad;
        global_sum_r += m.rocio;
        global_sum_p += m.presion;
        total_muestras++;

        pthread_mutex_unlock(&mutex_archivo);

        printf("[CONSUMIDOR-%s] Procesado: H:%d R:%d P:%d @ %s\n",
               estaciones_nombres[idx], m.humedad, m.rocio, m.presion, m.hora);
        fflush(stdout);
    }

    printf("[CONSUMIDOR-%s] Finalizado. Buffer vaciado.\n", estaciones_nombres[idx]);
    fflush(stdout);
    return NULL;
}

/* =====================================================================================
 * FUNCIÓN PRINCIPAL: main
 *
 * Orquesta el ciclo de vida completo del monitor:
 *   1. Parsear argumentos -b y -p con getopt
 *   2. Crear el Pipe Nominal (FIFO)
 *   3. Inicializar buffers, semáforos y mutexes
 *   4. Abrir el FIFO (bloquea hasta que el primer agente se conecte)
 *   5. Lanzar hilo recolector y NUM_ESTACIONES hilos consumidores
 *   6. [BUG 1 CORREGIDO] Esperar terminación con pthread_join (no pthread_cancel)
 *   7. Calcular promedios y emitir categoría meteorológica
 *   8. Liberar recursos (archivo, fd, pipe, semáforos, mutexes, memoria)
 * ===================================================================================== */
int main(int argc, char *argv[]) {
    int tamB = 0, opt;
    char *pNom = NULL;

    /* ------------------------------------------------------------------
     * Parseo de argumentos de línea de comandos con getopt.
     * Permite cualquier orden: -b 4 -p pipeNom  o  -p pipeNom -b 4
     * ------------------------------------------------------------------ */
    while ((opt = getopt(argc, argv, "b:p:")) != -1) {
        switch (opt) {
            case 'b':
                tamB = atoi(optarg); // Tamaño del buffer en número de mediciones
                break;
            case 'p':
                pNom = optarg;       // Nombre del pipe nominal (FIFO)
                break;
            default:
                fprintf(stderr, "Uso: ./monitor -b <tam_buffer> -p <nombre_pipe>\n");
                exit(EXIT_FAILURE);
        }
    }

    /* Validar que ambos argumentos obligatorios fueron proporcionados */
    if (!pNom || tamB <= 0) {
        fprintf(stderr, "Error: Argumentos insuficientes o inválidos.\n");
        fprintf(stderr, "Uso: ./monitor -b <tam_buffer> -p <nombre_pipe>\n");
        exit(EXIT_FAILURE);
    }

    /* ------------------------------------------------------------------
     * Creación del Pipe Nominal (FIFO / Named Pipe)
     *
     * unlink() elimina el FIFO si quedó residual de una ejecución anterior.
     * mkfifo() crea un nuevo FIFO con permisos 0666 (lectura/escritura para todos).
     * Sin el FIFO, los agentes no pueden enviar datos al monitor.
     * ------------------------------------------------------------------ */
    unlink(pNom); // Ignorar error: el FIFO puede no existir aún

    if (mkfifo(pNom, 0666) == -1) {
        perror("[ERROR CRÍTICO] No se pudo crear el Pipe Nominal");
        exit(EXIT_FAILURE);
    }

    /* Abrir archivo de salida consolidado para escritura */
    archivo_out = fopen("consolidado.csv", "w");
    if (!archivo_out) {
        perror("[ERROR CRÍTICO] No se pudo crear consolidado.csv");
        unlink(pNom);
        exit(EXIT_FAILURE);
    }

    /* ------------------------------------------------------------------
     * Inicialización de los buffers circulares y sus primitivas de sincronización.
     *
     * Cada buffer necesita:
     *   - Memoria dinámica para 'tamB' mediciones
     *   - Semáforo vacios inicializado en tamB (buffer completamente vacío)
     *   - Semáforo llenos inicializado en 0 (nada listo para consumir)
     *   - Mutex para proteger punteros inicio/fin/count
     * ------------------------------------------------------------------ */
    for (int i = 0; i < NUM_ESTACIONES; i++) {
        buffers[i].tamano    = tamB;
        buffers[i].datos     = malloc(sizeof(Medicion) * tamB);
        buffers[i].inicio    = 0;
        buffers[i].fin       = 0;
        buffers[i].count     = 0;
        buffers[i].finalizado = 0;

        if (!buffers[i].datos) {
            fprintf(stderr, "[ERROR] Fallo malloc para buffer %d\n", i);
            exit(EXIT_FAILURE);
        }

        /*
         * sem_init(sem, pshared, value):
         *   pshared=0 → semáforo compartido entre hilos del mismo proceso (no entre procesos)
         */
        sem_init(&buffers[i].vacios, 0, tamB); // Inicia con tamB espacios disponibles
        sem_init(&buffers[i].llenos, 0, 0);    // Inicia con 0 datos listos
        pthread_mutex_init(&buffers[i].mutex_buf, NULL);
    }

    printf("[MONITOR] Iniciado. Esperando agentes en pipe: %s\n", pNom);
    printf("[MONITOR] Tamaño de buffer por estación: %d mediciones\n", tamB);
    fflush(stdout);

    /* ------------------------------------------------------------------
     * Apertura del FIFO en modo solo lectura.
     *
     * IMPORTANTE: open() con O_RDONLY en un FIFO es BLOQUEANTE.
     * El proceso se detiene aquí hasta que al menos un agente abra el
     * mismo FIFO en modo escritura (O_WRONLY). Esto garantiza que el
     * monitor esté listo antes de que los agentes comiencen a enviar.
     * ------------------------------------------------------------------ */
    int fd = open(pNom, O_RDONLY);
    if (fd < 0) {
        perror("[ERROR CRÍTICO] No se pudo abrir el Pipe Nominal para lectura");
        fclose(archivo_out);
        unlink(pNom);
        exit(EXIT_FAILURE);
  }

    printf("[MONITOR] Agente conectado. Iniciando hilos del sistema...\n");
    fflush(stdout);

    /* ------------------------------------------------------------------
     * Creación de hilos del sistema
     * ------------------------------------------------------------------ */
    pthread_t rec;                        // Handle del hilo recolector
    pthread_t cons[NUM_ESTACIONES];       // Handles de los hilos consumidores
    int ids[NUM_ESTACIONES];              // IDs pasados como argumento a cada consumidor

    /* Lanzar el hilo recolector (productor del sistema) */
    if (pthread_create(&rec, NULL, hilo_recolector, &fd) != 0) {
        perror("[ERROR] No se pudo crear el hilo recolector");
        exit(EXIT_FAILURE);
    }

    /* Lanzar un hilo consumidor por cada estación */
    for (int i = 0; i < NUM_ESTACIONES; i++) {
        ids[i] = i;
        if (pthread_create(&cons[i], NULL, hilo_consumidor, &ids[i]) != 0) {
            fprintf(stderr, "[ERROR] No se pudo crear hilo consumidor %d\n", i);
            exit(EXIT_FAILURE);
        }
    }

    
    pthread_join(rec, NULL); 

    /* Esperar a que todos los consumidores terminen de vaciar sus buffers */
    for (int i = 0; i < NUM_ESTACIONES; i++) {
        pthread_join(cons[i], NULL);
    }

    printf("\n[MONITOR] Todos los hilos finalizaron correctamente.\n");
    fflush(stdout);

    /* ------------------------------------------------------------------
     * Cálculo de promedios y categorización meteorológica final.
     *
     * Se calculan los promedios globales de los tres parámetros y se
     * comparan con los umbrales de la Tabla 2 del enunciado para determinar
     * la categoría del clima en Bogotá.
     *
     * Tabla 2:
     *   Lluvioso: Humedad > 90  AND Rocío > 9  AND Presión < 750
     *   Nublado:  Humedad 80-95 AND Rocío > 8  AND Presión == 751
     *   Fresco:   Humedad < 80  AND Rocío 5-8  AND Presión > 754
     *   Variable: ninguna categoría anterior aplica
     * ------------------------------------------------------------------ */
    if (total_muestras > 0) {
        double avgH = global_sum_h / total_muestras; // Promedio humedad
        double avgR = global_sum_r / total_muestras; // Promedio rocío
        double avgP = global_sum_p / total_muestras; // Promedio presión

        printf("\n--- Control de Categorización Meteorológica ---\n");
        printf("Muestras procesadas: %d\n", total_muestras);
        printf("Promedios → Humedad: %.2f%% | Rocío: %.2f°C | Presión: %.2f hPa\n\n",
               avgH, avgR, avgP);

        printf("... Control de Categorización Meteorológica!!!\n");
        printf(" Parte Meteorológico Bogotá: ");

        /* Evaluación en orden: Lluvioso → Nublado → Fresco → Variable */
        if (avgH > 90 && avgR > 9 && avgP < 750) {
            printf("\"Lluvioso\"\n");
        } else if (avgH >= 80 && avgH <= 95 && avgR > 8 && (int)avgP == 751) {
            printf("\"Nublado\"\n");
        } else if (avgH < 80 && avgR >= 5 && avgR <= 8 && avgP > 754) {
            printf("\"Fresco\"\n");
        } else {
            printf("\"Variable\"\n");
        }

        printf("Fin del Monitor...!!!\n");
        fflush(stdout);

    } else {
        /* Caso borde: ningún agente envió datos válidos */
        printf("[MONITOR] Advertencia: No se procesaron mediciones.\n");
        printf("Verifique que los archivos de sensor tengan datos en rango válido.\n");
    }

    /* ------------------------------------------------------------------
     * Liberación de recursos (buenas prácticas POSIX)
     *
     * Es importante liberar todos los recursos en orden inverso a su creación:
     * archivo → fd → pipe → semáforos → mutexes → memoria dinámica
     * ------------------------------------------------------------------ */
    printf("\n[MONITOR] Liberando recursos del sistema...\n");

    fclose(archivo_out);  // Cerrar archivo CSV
    close(fd);            // Cerrar descriptor del FIFO
    unlink(pNom);         // Eliminar el FIFO del sistema de archivos

    /* Destruir semáforos, mutexes y liberar memoria de cada buffer */
    for (int i = 0; i < NUM_ESTACIONES; i++) {
        sem_destroy(&buffers[i].vacios);
        sem_destroy(&buffers[i].llenos);
        pthread_mutex_destroy(&buffers[i].mutex_buf);
        free(buffers[i].datos);
    }

    pthread_mutex_destroy(&mutex_archivo);

    printf("[MONITOR] Sistema cerrado limpiamente.\n");
    return 0;
}
