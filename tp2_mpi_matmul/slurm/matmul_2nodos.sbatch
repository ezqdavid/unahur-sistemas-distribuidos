#!/bin/bash
#SBATCH --job-name=matmul_mpi_cronos
#SBATCH --output=logs/matmul_%j.out
#SBATCH --error=logs/matmul_%j.err
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=4
#SBATCH --time=00:10:00

# ==============================================================================
# Cronos 2026 - Script Batch Slurm para Multiplicación de Matrices MPI en C
# Ejecuta en 2 nodos de cómputo (4 procesos MPI por nodo = 8 procesos totales)
# ==============================================================================

set -euo pipefail

mkdir -p logs

echo "============================================================"
echo " Job ID       : ${SLURM_JOB_ID}"
echo " Nodos        : ${SLURM_JOB_NODELIST}"
echo " Total tareas : ${SLURM_NTASKS}"
echo " Directorio   : $(pwd)"
echo " Fecha inicio : $(date)"
echo "============================================================"

# Compilar si el binario no existe o si el código fuente es más reciente
if [ ! -f ./matmul_mpi ] || [ src/matmul_mpi.c -nt ./matmul_mpi ]; then
    echo "[INFO] Compilando matmul_mpi..."
    make
fi

# Tamaño de la matriz (por defecto 2048x2048; se puede pasar por argumento a sbatch)
MATRIX_SIZE=${1:-2048}

echo "[INFO] Ejecutando multiplicación de matrices ${MATRIX_SIZE}x${MATRIX_SIZE}..."
srun --mpi=pmix ./matmul_mpi "${MATRIX_SIZE}"

echo "[INFO] Fecha fin: $(date)"
