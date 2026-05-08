# #######################################################################################
# # Pontificia Universidad Javeriana
# # Fecha: 2026
# # Autores: Oscar Pinilla, David Pedraza, Johan Barreto
# # Proyecto: Sistema de Monitoreo Meteorológico (IPC + Threads)
# # Descripción: Makefile para la gestión de compilación automatizada. 
# #              Configura banderas de compilación, enlaces de bibliotecas y 
# #              reglas de limpieza del entorno.
# #######################################################################################

# --- Variables de Compilación ---
CC = gcc
# CFLAGS: -Wall y -Wextra activan todos los avisos del compilador para asegurar un código robusto.
# -g permite la depuración con herramientas como GDB.
# -I especifica el directorio de los archivos de cabecera (.h).
CFLAGS = -Wall -Wextra -g -I./include
# LDFLAGS: Enlaza la biblioteca de hilos POSIX necesaria para el Monitor.
LDFLAGS = -lpthread

# --- Directorios del Proyecto ---
BIN_DIR = ./bin
SRC_DIR = ./src

# --- Reglas de Compilación ---

# Objetivo principal: Construye ambos ejecutables
all: $(BIN_DIR)/monitor $(BIN_DIR)/agenteM

# Regla para el Monitor:
# Requiere el código fuente del monitor. Se vincula con la librería de hilos.
$(BIN_DIR)/monitor: $(SRC_DIR)/monitor.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(SRC_DIR)/monitor.c -o $(BIN_DIR)/monitor $(LDFLAGS)

# Regla para el Agente:
# Compila el agente junto con el módulo de validaciones para asegurar que la lógica
# de filtrado esté integrada en el binario.
$(BIN_DIR)/agenteM: $(SRC_DIR)/agenteM.c $(SRC_DIR)/validaciones.c
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(SRC_DIR)/agenteM.c $(SRC_DIR)/validaciones.c -o $(BIN_DIR)/agenteM

# --- Reglas de Mantenimiento ---

# Clean: Elimina los archivos binarios, el archivo consolidado y el pipe nominal.
# Garantiza un entorno de compilación limpio para evitar errores por versiones antiguas.
clean:
	rm -f $(BIN_DIR)/* consolidado.csv pipeNominal
	@echo "Limpieza completada: Binarios, CSV y Pipes eliminados."

# Ayuda: Muestra las instrucciones de uso del Makefile
help:
	@echo "Opciones disponibles:"
	@echo "  make all    - Compila monitor y agenteM"
	@echo "  make clean  - Elimina ejecutables y archivos temporales"