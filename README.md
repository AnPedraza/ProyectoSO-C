## Sistema de Monitoreo Meteorológico 
**Pontificia Universidad Javeriana**  
**Facultad de Ingeniería - Sistemas Operativos / Proyecto-C**


##Integrantes
*Oscar Pinilla* - [Perfil de GitHub](https://github.com/oscar-Pinilla)
*David Pedraza* - [Perfil de GitHub](https://github.com/AnPedraza)
*Johan Barreto* - [Perfil de GitHub](https://github.com/Johanb4)



##Descripción del Proyecto
Este sistema simula una red de estaciones meteorológicas en Bogotá (Usaquén, Teusaquillo y Kennedy) que envían datos en tiempo real a un servidor central de monitoreo. El proyecto pone en práctica conceptos avanzados de **programación de sistemas**, **concurrencia** e **IPC (Inter-Process Communication)**.

El flujo de datos sigue el patrón (Productor-Consumidor), asegurando que la información se procese de manera ordenada y sin pérdida de integridad.


##  Arquitectura Técnica

### 1. Agente de Mediciones (`agenteM`)
* **Función:** Actúa como el proceso productor.
* **Validación:** Filtra mediciones basadas en la Tabla 1 (Rangos lógicos de humedad, presión y rocío) antes de enviarlas.
* **Comunicación:** Utiliza un **Pipe Nominal (FIFO)** para transmitir cadenas de texto formateadas al Monitor.

### 2. Monitor Central (`monitor`)
* **Hilo Recolector:** Lee continuamente del Pipe y distribuye las mediciones según la estación.
* **Buffer Circular:** Cada estación posee su propio buffer circular de tamaño configurable para manejar ráfagas de datos.
* **Hilos Consumidores:** Múltiples hilos (uno por estación) procesan los datos de los buffers, calculan estadísticas y escriben en el archivo final.
* **Sincronización:** Implementada mediante **Semáforos POSIX** (`sem_wait`, `sem_post`) y **Mutex** para proteger las secciones críticas (archivo CSV y variables globales).


## Guía de Ejecución

### Ejecución paso a paso

#### 1. Iniciar el Monitor

Compila los binarios usando el `Makefile` incluido:

```bash
make all
```

Luego ejecuta el monitor:

```bash
./bin/monitor -b 20 -p pipeNominal
```

| Parámetro | Descripción |
|-----------|-------------|
| `-b` | Tamaño del buffer circular (ej. `20` espacios) |
| `-p` | Nombre del pipe para la comunicación |

---

#### 2. Iniciar los Agentes

Abre una terminal adicional por cada estación:

**Estación Kennedy**
```bash
./bin/agenteM -f sensorken.csv -t 1 -p pipeNominal
```

**Estación Teusaquillo**
```bash
./bin/agenteM -f sensorteu.csv -t 2 -p pipeNominal
```

| Parámetro | Descripción |
|-----------|-------------|
| `-f` | Archivo fuente de datos (CSV) |
| `-t` | Tiempo de espera entre envíos (segundos) |
