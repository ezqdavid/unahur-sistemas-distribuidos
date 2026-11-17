#!/bin/bash
# Verificación rápida del stack Cronos 2026 (Raspberry Pi 5 · Debian 13 · ARM64)
set -e

echo "=== Hostname y Arquitectura ==="
hostname
uname -m

echo -e "\n=== Compiladores (GCC / G++ / GFortran / Make) ==="
gcc --version | head -n 1
g++ --version | head -n 1
gfortran --version | head -n 1
make --version | head -n 1

echo -e "\n=== OpenMPI ==="
mpicc --version | head -n 1
mpirun --version | head -n 1

echo -e "\n=== Python 3 y Bibliotecas Científicas ==="
python3 --version
python3 -c "
import numpy, scipy, pandas, matplotlib, mpi4py
print('NumPy:', numpy.__version__)
print('SciPy:', scipy.__version__)
print('pandas:', pandas.__version__)
print('Matplotlib:', matplotlib.__version__)
print('mpi4py:', mpi4py.__version__)
"

echo -e "\n=== Slurm y PMIx ==="
sinfo -N -l || true
srun --mpi=list || true
