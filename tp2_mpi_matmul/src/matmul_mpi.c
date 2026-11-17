/**
 * @file matmul_mpi.c
 * @brief Multiplicación paralela distribuida de matrices de números grandes con MPI en C.
 *
 * Diseñado para el clúster Cronos 2026 (Raspberry Pi 5, Debian 13 ARM64, OpenMPI 5.x, Slurm + PMIx).
 *
 * Estrategia de paralelización:
 *  - Descomposición 1D por filas de la matriz A y la matriz resultado C.
 *  - Distribución balanceada mediante MPI_Scatterv / MPI_Gatherv (soporta cualquier N,
 *    incluso cuando N no es divisible por la cantidad de procesos MPI).
 *  - Difusión de la matriz B completa a todos los procesos con MPI_Bcast.
 *  - Cómputo local optimizado para caché (orden de bucles i-k-j) usando tipo double (64 bits)
 *    para operar con números grandes sin desbordamiento de enteros de 32 bits.
 *
 * Compilación:
 *   mpicc -O3 -Wall -Wextra -std=c11 -o matmul_mpi src/matmul_mpi.c
 *
 * Ejecución:
 *   srun --mpi=pmix ./matmul_mpi 2048
 */

#include <math.h>
#include <mpi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_N 2048
#define VERIFY_SAMPLES 16

/**
 * Inicializa las matrices A y B con valores grandes deterministas.
 * Rango de cada elemento: [100000, 999999].
 * El producto A[i][k] * B[k][j] (~10^10 a 10^12) desbordaría un int de 32 bits,
 * pero se representa con exactitud en IEEE-754 double (hasta 2^53 ≈ 9e15).
 */
static void init_matrices(double *A, double *B, int n) {
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < n; k++) {
            A[(size_t)i * n + k] = (double)(100000 + ((i * 31 + k * 17) % 900000));
            B[(size_t)i * n + k] = (double)(100000 + ((i * 13 + k * 29) % 900000));
        }
    }
}

/**
 * Calcula los arreglos de conteo (en elementos) y desplazamientos para
 * MPI_Scatterv y MPI_Gatherv, repartiendo las N filas lo más equitativamente posible.
 */
static void build_row_distribution(int n, int size, int *sendcounts, int *displs) {
    int base_rows = n / size;
    int remainder = n % size;
    int offset = 0;

    for (int r = 0; r < size; r++) {
        int rows = base_rows + (r < remainder ? 1 : 0);
        sendcounts[r] = rows * n;
        displs[r] = offset;
        offset += rows * n;
    }
}

/**
 * Multiplicación local C_local = A_local * B usando orden de bucles i-k-j.
 * Recorrer B y C de forma contigua en memoria aprovecha la localidad espacial
 * y las líneas de caché L1/L2 de la arquitectura ARM64 (Cortex-A76).
 */
static void matmul_local_ikj(const double * restrict A_local,
                             const double * restrict B,
                             double * restrict C_local,
                             int local_rows,
                             int n) {
    memset(C_local, 0, (size_t)local_rows * n * sizeof(double));

    for (int i = 0; i < local_rows; i++) {
        const double *a_row = &A_local[(size_t)i * n];
        double *c_row = &C_local[(size_t)i * n];

        for (int k = 0; k < n; k++) {
            double a_ik = a_row[k];
            const double *b_row = &B[(size_t)k * n];

            for (int j = 0; j < n; j++) {
                c_row[j] += a_ik * b_row[j];
            }
        }
    }
}

/**
 * Verifica una muestra distribuida de elementos de C contra el cálculo secuencial
 * exacto en el proceso raíz (O(VERIFY_SAMPLES * N), muy rápido).
 */
static int verify_samples(const double *A, const double *B, const double *C, int n, double *max_err) {
    *max_err = 0.0;
    int step_i = (n > VERIFY_SAMPLES) ? (n / VERIFY_SAMPLES) : 1;

    for (int s = 0; s < VERIFY_SAMPLES && s * step_i < n; s++) {
        int i = s * step_i;
        int j = (s * 97 + 13) % n;
        double expected = 0.0;

        for (int k = 0; k < n; k++) {
            expected += A[(size_t)i * n + k] * B[(size_t)k * n + j];
        }

        double diff = fabs(C[(size_t)i * n + j] - expected);
        if (diff > *max_err) {
            *max_err = diff;
        }
        if (diff > 1e-3 * fabs(expected) && diff > 1e-6) {
            return 0;
        }
    }
    return 1;
}

static void *xmalloc_or_abort(size_t bytes, const char *label, int rank) {
    void *ptr = malloc(bytes);
    if (!ptr) {
        fprintf(stderr, "[Rank %d] Error: no se pudo reservar %zu bytes para %s\n",
                rank, bytes, label);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    return ptr;
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank = 0;
    int size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int n = DEFAULT_N;
    int do_verify = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-verify") == 0) {
            do_verify = 0;
        } else {
            int parsed = atoi(argv[i]);
            if (parsed > 0) {
                n = parsed;
            } else if (rank == 0) {
                fprintf(stderr, "Uso: %s [N] [--no-verify]\n", argv[0]);
                MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
            }
        }
    }

    /* Validar que N*N no desborde el tipo int usado por los conteos de MPI */
    if ((long long)n * n > 2000000000LL) {
        if (rank == 0) {
            fprintf(stderr, "Error: N=%d excede el límite de elementos por mensaje MPI.\n", n);
        }
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    char processor_name[MPI_MAX_PROCESSOR_NAME];
    int name_len = 0;
    MPI_Get_processor_name(processor_name, &name_len);

    int base_rows = n / size;
    int remainder = n % size;
    int local_rows = base_rows + (rank < remainder ? 1 : 0);

    double *A = NULL;
    double *C = NULL;
    int *sendcounts = NULL;
    int *displs = NULL;

    /* Todos los procesos necesitan B completa, su porción A_local y su porción C_local */
    double *B = xmalloc_or_abort((size_t)n * n * sizeof(double), "B", rank);
    double *A_local = xmalloc_or_abort((size_t)(local_rows > 0 ? local_rows : 1) * n * sizeof(double),
                                       "A_local", rank);
    double *C_local = xmalloc_or_abort((size_t)(local_rows > 0 ? local_rows : 1) * n * sizeof(double),
                                       "C_local", rank);

    if (rank == 0) {
        A = xmalloc_or_abort((size_t)n * n * sizeof(double), "A", rank);
        C = xmalloc_or_abort((size_t)n * n * sizeof(double), "C", rank);
        sendcounts = xmalloc_or_abort((size_t)size * sizeof(int), "sendcounts", rank);
        displs = xmalloc_or_abort((size_t)size * sizeof(int), "displs", rank);

        init_matrices(A, B, n);
        build_row_distribution(n, size, sendcounts, displs);
    }

    /* Sincronización previa para medir tiempos de forma justa */
    MPI_Barrier(MPI_COMM_WORLD);
    double t_start = MPI_Wtime();

    /* 1. Distribución de datos (B a todos, filas de A repartidas) */
    MPI_Bcast(B, n * n, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Scatterv(A, sendcounts, displs, MPI_DOUBLE,
                 A_local, local_rows * n, MPI_DOUBLE,
                 0, MPI_COMM_WORLD);

    double t_comp_start = MPI_Wtime();

    /* 2. Cómputo paralelo local */
    if (local_rows > 0) {
        matmul_local_ikj(A_local, B, C_local, local_rows, n);
    }

    double t_comp_end = MPI_Wtime();

    /* 3. Recolección de resultados en el proceso raíz */
    MPI_Gatherv(C_local, local_rows * n, MPI_DOUBLE,
                C, sendcounts, displs, MPI_DOUBLE,
                0, MPI_COMM_WORLD);

    double t_end = MPI_Wtime();

    /* Reporte de ejecución por proceso para evidenciar distribución multi-nodo */
    printf("[Rank %d/%d @ %s] Filas procesadas: %d | Cómputo local: %.4f s\n",
           rank, size, processor_name, local_rows, t_comp_end - t_comp_start);

    MPI_Barrier(MPI_COMM_WORLD);

    if (rank == 0) {
        double total_time = t_end - t_start;
        double comp_time = t_comp_end - t_comp_start;
        double comm_time = total_time - comp_time;
        double flops = 2.0 * (double)n * (double)n * (double)n;
        double gflops_total = flops / (total_time * 1e9);
        double gflops_comp = flops / (comp_time * 1e9);

        printf("\n============================================================\n");
        printf(" Cronos 2026 - Multiplicación de Matrices MPI (N = %d)\n", n);
        printf("============================================================\n");
        printf(" Procesos MPI       : %d\n", size);
        printf(" Memoria matrices   : %.2f MB (A + B + C)\n",
               (3.0 * (double)n * (double)n * sizeof(double)) / (1024.0 * 1024.0));
        printf(" Tiempo total       : %.4f s\n", total_time);
        printf(" Tiempo cómputo     : %.4f s\n", comp_time);
        printf(" Tiempo comunicación: %.4f s\n", comm_time);
        printf(" Rendimiento total  : %.3f GFLOPS\n", gflops_total);
        printf(" Rendimiento cómputo: %.3f GFLOPS\n", gflops_comp);
        printf(" Muestra C[0][0]    : %.0f\n", C[0]);
        printf(" Muestra C[N-1][N-1]: %.0f\n", C[(size_t)(n - 1) * n + (n - 1)]);

        if (do_verify) {
            double max_err = 0.0;
            int ok = verify_samples(A, B, C, n, &max_err);
            printf(" Verificación       : %s (error máx: %.3e)\n",
                   ok ? "PASS" : "FAIL", max_err);
        }
        printf("============================================================\n");

        free(A);
        free(C);
        free(sendcounts);
        free(displs);
    }

    free(B);
    free(A_local);
    free(C_local);

    MPI_Finalize();
    return 0;
}
