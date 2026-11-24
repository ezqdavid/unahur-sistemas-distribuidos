# Multiplicación de Matrices con MPI en C (Cronos 2026)

Implementación paralela híbrida (**MPI + OpenMP + OpenBLAS**) para multiplicación de matrices cuadradas de números grandes (`double` de 64 bits, sin desbordamiento de `int32`), diseñada para ejecutarse en múltiples nodos de cómputo bajo Slurm con PMIx.

## Estructura

```text
.
├── Makefile
├── README.md
├── src/matmul_mpi.c
└── slurm/matmul.sbatch
```

## Compilación

`Makefile` detecta automáticamente `libopenblas-dev`; si no está presente, compila usando el kernel paralelo OpenMP + cache tiling (`64x64`).

```bash
make
```

Para forzar el kernel OpenMP puro sin OpenBLAS:

```bash
make clean && make USE_OPENBLAS=0
```

## Ejecución en Slurm

Desde la raíz del proyecto en el directorio compartido del clúster:

```bash
sbatch slurm/matmul.sbatch
sbatch slurm/matmul.sbatch 6000
sbatch --nodes=4 slurm/matmul.sbatch 8192
```

El script solicita `--ntasks-per-node=1 --cpus-per-task=4` para compartir una sola copia de los buffers por nodo y evitar contención de red TCP.

## Selección adaptativa de estrategia (`execute_matmul`)

Antes de reservar memoria, el programa consulta `MemAvailable` (`/proc/meminfo`), detecta los rangos por nodo (`MPI_COMM_TYPE_SHARED`), ajusta los hilos de OpenMP/OpenBLAS para evitar *oversubscription* y selecciona automáticamente la estrategia:

| Estrategia | Condición (`--strategy=auto`) | Diseño |
| :--- | :--- | :--- |
| `LOCAL` | `size == 1` o `N <= 512` | Ejecución local en el rango 0 con hilos locales, sin overhead de red. |
| `1D_SCATTERV` | `512 < N <= 4096` y `3*N^2*8` cabe en RAM segura | `MPI_Bcast(B)` + `MPI_Scatterv(A)` + `MPI_Gatherv(C)`. |
| `RING_ASYNC` | `N > 4096` o excede `1D`, si filas locales + 2 paneles caben en RAM | Generación distribuida de `A` y paneles de `B`; rotación asíncrona en anillo (`MPI_Irecv`/`MPI_Isend`) con doble buffering. |
| `OUT_OF_CORE_STREAM` | Los paneles de `RING_ASYNC` exceden la RAM segura (ej. `N = 100000`) | Streaming cíclico por bloques `b x b` acotados en RAM. |

## Opciones CLI

```text
./matmul_mpi [N] [--strategy=auto|local|1d|ring|stream] [--plan-only] [--no-verify]
```

- `N`: dimensión de las matrices `N x N` (por defecto `2048`).
- `--strategy=...`: fuerza una estrategia específica para comparativas.
- `--plan-only`: inspecciona hardware, RAM segura y estrategia seleccionada sin ejecutar el cálculo.
- `--no-verify`: omite la verificación de muestras respecto al producto exacto.
