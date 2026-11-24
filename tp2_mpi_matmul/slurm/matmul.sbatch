#!/bin/bash
#SBATCH --job-name=matmul_mpi
#SBATCH --output=logs/matmul_%j.out
#SBATCH --error=logs/matmul_%j.err
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --time=00:15:00

set -euo pipefail

mkdir -p logs

if [ ! -f ./matmul_mpi ] || [ src/matmul_mpi.c -nt ./matmul_mpi ]; then
    make
fi

export OMPI_MCA_btl_tcp_if_exclude=lo,docker0

srun --mpi=pmix ./matmul_mpi "${1:-2048}" "${@:2}"
