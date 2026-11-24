#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <mpi.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_OPENBLAS
#include <cblas.h>
void openblas_set_num_threads(int num_threads);
#endif

#define DEFAULT_N 2048
#define TILE_SIZE 64
#define VERIFY_SAMPLES 16
#define MPI_COUNT_LIMIT 2000000000LL

typedef enum {
    STRAT_AUTO = 0,
    STRAT_LOCAL,
    STRAT_1D,
    STRAT_RING,
    STRAT_STREAM
} Strategy;

typedef struct {
    uint64_t mem_avail;
    uint64_t min_safe_ram;
    int local_size;
    int threads;
    int block_size;
} EnvInfo;

typedef struct {
    double total_time;
    double comm_time;
    double comp_time;
    double c_first;
    double c_last;
    double max_err;
    int verify_ok;
    uint64_t est_rank_bytes;
} RunStats;

static inline double elem_a(int i, int k) {
    return (double)(100000 + (((long long)i * 31 + (long long)k * 17) % 900000));
}

static inline double elem_b(int k, int j) {
    return (double)(100000 + (((long long)k * 13 + (long long)j * 29) % 900000));
}

static int split_count(int n, int size, int rank) {
    return (n / size) + (rank < (n % size) ? 1 : 0);
}

static int split_offset(int n, int size, int rank) {
    int rem = n % size;
    return rank * (n / size) + (rank < rem ? rank : rem);
}

static const char *strategy_name(Strategy s) {
    switch (s) {
    case STRAT_LOCAL:  return "LOCAL";
    case STRAT_1D:     return "1D_SCATTERV";
    case STRAT_RING:   return "RING_ASYNC";
    case STRAT_STREAM: return "OUT_OF_CORE_STREAM";
    default:           return "AUTO";
    }
}

static void *xmalloc(size_t bytes, const char *label, int rank) {
    void *p = malloc(bytes > 0 ? bytes : 1);
    if (!p) {
        fprintf(stderr, "[Rank %d] Out of memory allocating %zu bytes (%s)\n",
                rank, bytes, label);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }
    return p;
}

static uint64_t read_mem_available(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        return 1024ULL * 1024ULL * 1024ULL;
    }
    char key[64];
    unsigned long long val_kb = 0;
    uint64_t avail = 1024ULL * 1024ULL * 1024ULL;
    while (fscanf(f, "%63s %llu %*s", key, &val_kb) == 2) {
        if (strcmp(key, "MemAvailable:") == 0) {
            avail = (uint64_t)val_kb * 1024ULL;
            break;
        }
    }
    fclose(f);
    return avail;
}

static int select_block_size(int n, uint64_t min_safe_ram) {
    int cap = n > 128 ? n / 2 : n;
    if (cap > 2048) {
        cap = 2048;
    }
    int b = (cap / 64) * 64;
    if (b < 64) {
        b = n < 64 ? n : 64;
    }
    while (b > 64 && 3ULL * (uint64_t)b * (uint64_t)b * sizeof(double) > min_safe_ram / 2) {
        b -= 64;
    }
    return b;
}

static EnvInfo inspect_env(int n, int rank) {
    EnvInfo env;
    env.mem_avail = read_mem_available();

    MPI_Comm shm;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &shm);
    MPI_Comm_size(shm, &env.local_size);
    MPI_Comm_free(&shm);

    uint64_t local_safe = (uint64_t)((double)env.mem_avail * 0.75) / (uint64_t)env.local_size;
    MPI_Allreduce(&local_safe, &env.min_safe_ram, 1, MPI_UINT64_T, MPI_MIN, MPI_COMM_WORLD);

    int procs = omp_get_num_procs();
    env.threads = (procs / env.local_size) > 1 ? (procs / env.local_size) : 1;
    omp_set_num_threads(env.threads);
#ifdef USE_OPENBLAS
    openblas_set_num_threads(env.threads);
#endif

    env.block_size = select_block_size(n, env.min_safe_ram);

    if (rank == 0 && n > 9000) {
        fprintf(stderr,
                "[WARN] N=%d: C_ij exceeds exact 53-bit integer range in double (2^53); "
                "using IEEE-754 floating-point precision.\n", n);
    }
    return env;
}

static void dgemm_kernel(const double * restrict A,
                         const double * restrict B,
                         double * restrict C,
                         int m, int n, int k, int ldc, double beta) {
    if (m <= 0 || n <= 0 || k <= 0) {
        return;
    }
#ifdef USE_OPENBLAS
    cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                m, n, k, 1.0, A, k, B, n, beta, C, ldc);
#else
    if (beta == 0.0) {
        for (int i = 0; i < m; i++) {
            memset(&C[(size_t)i * ldc], 0, (size_t)n * sizeof(double));
        }
    }
    #pragma omp parallel for collapse(2) schedule(static)
    for (int ii = 0; ii < m; ii += TILE_SIZE) {
        for (int jj = 0; jj < n; jj += TILE_SIZE) {
            int i_max = (ii + TILE_SIZE < m) ? ii + TILE_SIZE : m;
            int j_max = (jj + TILE_SIZE < n) ? jj + TILE_SIZE : n;
            for (int kk = 0; kk < k; kk += TILE_SIZE) {
                int k_max = (kk + TILE_SIZE < k) ? kk + TILE_SIZE : k;
                for (int i = ii; i < i_max; i++) {
                    const double *a_row = &A[(size_t)i * k];
                    double *c_row = &C[(size_t)i * ldc];
                    for (int p = kk; p < k_max; p++) {
                        double a_ip = a_row[p];
                        const double *b_row = &B[(size_t)p * n];
                        for (int j = jj; j < j_max; j++) {
                            c_row[j] += a_ip * b_row[j];
                        }
                    }
                }
            }
        }
    }
#endif
}

static double exact_dot(int i, int j, int n) {
    double sum = 0.0;
    for (int k = 0; k < n; k++) {
        sum += elem_a(i, k) * elem_b(k, j);
    }
    return sum;
}

static int check_value(int i, int j, int n, double got, double *max_err) {
    double expected = exact_dot(i, j, n);
    double diff = fabs(got - expected);
    if (diff > *max_err) {
        *max_err = diff;
    }
    return !(diff > 1e-6 * fabs(expected) && diff > 1e-6);
}

static void get_sample_coords(int n, int s, int *si, int *sj) {
    int step = (n > VERIFY_SAMPLES) ? (n / VERIFY_SAMPLES) : 1;
    *si = s * step;
    *sj = (s * 97 + 13) % n;
}

static uint64_t estimate_rank_bytes(Strategy s, int n, int size, const EnvInfo *env) {
    int max_slice = split_count(n, size, 0);
    switch (s) {
    case STRAT_LOCAL:
        return 3ULL * (uint64_t)n * (uint64_t)n * sizeof(double);
    case STRAT_1D:
        return 3ULL * (uint64_t)n * (uint64_t)n * sizeof(double);
    case STRAT_RING:
        return (uint64_t)max_slice * (uint64_t)n * 16ULL +
               2ULL * (uint64_t)max_slice * (uint64_t)n * 8ULL;
    case STRAT_STREAM:
        return 3ULL * (uint64_t)env->block_size * (uint64_t)env->block_size * sizeof(double);
    default:
        return 0;
    }
}

static Strategy select_strategy(int n, int size, const EnvInfo *env, Strategy forced, int rank) {
    if (forced != STRAT_AUTO) {
        if (forced == STRAT_1D && (long long)n * n > MPI_COUNT_LIMIT) {
            if (rank == 0) {
                fprintf(stderr, "Error: N=%d exceeds MPI int count limit for 1D strategy.\n", n);
            }
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
        if (forced == STRAT_RING && (long long)split_count(n, size, 0) * n > MPI_COUNT_LIMIT) {
            if (rank == 0) {
                fprintf(stderr, "Error: N=%d exceeds MPI int count limit for RING strategy.\n", n);
            }
            MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
        }
        return forced;
    }

    if (size == 1 || n <= 512) {
        return STRAT_LOCAL;
    }

    uint64_t bytes_1d = estimate_rank_bytes(STRAT_1D, n, size, env);
    if (n <= 4096 && bytes_1d < env->min_safe_ram && (long long)n * n <= MPI_COUNT_LIMIT) {
        return STRAT_1D;
    }

    int max_slice = split_count(n, size, 0);
    uint64_t bytes_ring = estimate_rank_bytes(STRAT_RING, n, size, env);
    if (bytes_ring < env->min_safe_ram && (long long)max_slice * n <= MPI_COUNT_LIMIT) {
        return STRAT_RING;
    }

    return STRAT_STREAM;
}

static void run_local(int n, int rank, int verify, RunStats *st) {
    double t0 = MPI_Wtime();
    if (rank == 0) {
        size_t nn = (size_t)n * n;
        double *A = xmalloc(nn * sizeof(double), "A", rank);
        double *B = xmalloc(nn * sizeof(double), "B", rank);
        double *C = xmalloc(nn * sizeof(double), "C", rank);

        #pragma omp parallel for schedule(static)
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < n; k++) {
                A[(size_t)i * n + k] = elem_a(i, k);
                B[(size_t)i * n + k] = elem_b(i, k);
            }
        }

        double tc0 = MPI_Wtime();
        dgemm_kernel(A, B, C, n, n, n, n, 0.0);
        double tc1 = MPI_Wtime();

        st->comp_time = tc1 - tc0;
        st->c_first = C[0];
        st->c_last = C[nn - 1];

        if (verify) {
            for (int s = 0; s < VERIFY_SAMPLES; s++) {
                int si, sj;
                get_sample_coords(n, s, &si, &sj);
                if (si < n && !check_value(si, sj, n, C[(size_t)si * n + sj], &st->max_err)) {
                    st->verify_ok = 0;
                }
            }
        }
        free(A);
        free(B);
        free(C);
    }
    MPI_Barrier(MPI_COMM_WORLD);
    double t1 = MPI_Wtime();
    st->total_time = t1 - t0;
    st->comm_time = st->total_time - st->comp_time;
}

static void run_1d(int n, int rank, int size, int verify, RunStats *st) {
    int local_rows = split_count(n, size, rank);
    size_t nn = (size_t)n * n;
    size_t loc_elems = (size_t)(local_rows > 0 ? local_rows : 1) * n;

    double *B = xmalloc(nn * sizeof(double), "B", rank);
    double *A_loc = xmalloc(loc_elems * sizeof(double), "A_loc", rank);
    double *C_loc = xmalloc(loc_elems * sizeof(double), "C_loc", rank);
    double *A = NULL, *C = NULL;
    int *counts = NULL, *displs = NULL;

    if (rank == 0) {
        A = xmalloc(nn * sizeof(double), "A", rank);
        C = xmalloc(nn * sizeof(double), "C", rank);
        counts = xmalloc((size_t)size * sizeof(int), "counts", rank);
        displs = xmalloc((size_t)size * sizeof(int), "displs", rank);

        #pragma omp parallel for schedule(static)
        for (int i = 0; i < n; i++) {
            for (int k = 0; k < n; k++) {
                A[(size_t)i * n + k] = elem_a(i, k);
                B[(size_t)i * n + k] = elem_b(i, k);
            }
        }
        for (int r = 0; r < size; r++) {
            counts[r] = split_count(n, size, r) * n;
            displs[r] = split_offset(n, size, r) * n;
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();

    MPI_Bcast(B, n * n, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Scatterv(A, counts, displs, MPI_DOUBLE,
                 A_loc, local_rows * n, MPI_DOUBLE, 0, MPI_COMM_WORLD);

    double tc0 = MPI_Wtime();
    dgemm_kernel(A_loc, B, C_loc, local_rows, n, n, n, 0.0);
    double tc1 = MPI_Wtime();

    MPI_Gatherv(C_loc, local_rows * n, MPI_DOUBLE,
                C, counts, displs, MPI_DOUBLE, 0, MPI_COMM_WORLD);

    double t1 = MPI_Wtime();

    double local_comp = tc1 - tc0;
    MPI_Reduce(&local_comp, &st->comp_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        st->total_time = t1 - t0;
        st->comm_time = st->total_time - st->comp_time;
        st->c_first = C[0];
        st->c_last = C[nn - 1];

        if (verify) {
            for (int s = 0; s < VERIFY_SAMPLES; s++) {
                int si, sj;
                get_sample_coords(n, s, &si, &sj);
                if (si < n && !check_value(si, sj, n, C[(size_t)si * n + sj], &st->max_err)) {
                    st->verify_ok = 0;
                }
            }
        }
        free(A);
        free(C);
        free(counts);
        free(displs);
    }
    free(B);
    free(A_loc);
    free(C_loc);
}

static void run_ring(int n, int rank, int size, int verify, RunStats *st) {
    int my_rows = split_count(n, size, rank);
    int my_row_off = split_offset(n, size, rank);
    int my_cols = split_count(n, size, rank);
    int my_col_off = split_offset(n, size, rank);
    int max_cols = split_count(n, size, 0);

    size_t row_elems = (size_t)(my_rows > 0 ? my_rows : 1) * n;
    size_t panel_elems = (size_t)n * max_cols;

    double *A_loc = xmalloc(row_elems * sizeof(double), "A_loc", rank);
    double *C_loc = xmalloc(row_elems * sizeof(double), "C_loc", rank);
    double *B_curr = xmalloc(panel_elems * sizeof(double), "B_curr", rank);
    double *B_next = xmalloc(panel_elems * sizeof(double), "B_next", rank);

    #pragma omp parallel for schedule(static)
    for (int i = 0; i < my_rows; i++) {
        for (int k = 0; k < n; k++) {
            A_loc[(size_t)i * n + k] = elem_a(my_row_off + i, k);
        }
    }

    if (my_cols > 0) {
        #pragma omp parallel for schedule(static)
        for (int k = 0; k < n; k++) {
            for (int j = 0; j < my_cols; j++) {
                B_curr[(size_t)k * my_cols + j] = elem_b(k, my_col_off + j);
            }
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();
    double comp_acc = 0.0;

    int prev = (rank - 1 + size) % size;
    int next = (rank + 1) % size;

    for (int step = 0; step < size; step++) {
        int cur_owner = (rank + step) % size;
        int cur_cols = split_count(n, size, cur_owner);
        int cur_col_off = split_offset(n, size, cur_owner);
        MPI_Request reqs[2];

        if (step < size - 1) {
            int next_owner = (rank + step + 1) % size;
            int next_cols = split_count(n, size, next_owner);
            MPI_Irecv(B_next, next_cols * n, MPI_DOUBLE, next, step, MPI_COMM_WORLD, &reqs[0]);
            MPI_Isend(B_curr, cur_cols * n, MPI_DOUBLE, prev, step, MPI_COMM_WORLD, &reqs[1]);
        }

        double tc0 = MPI_Wtime();
        if (my_rows > 0 && cur_cols > 0) {
            dgemm_kernel(A_loc, B_curr, C_loc + cur_col_off,
                         my_rows, cur_cols, n, n, 0.0);
        }
        comp_acc += MPI_Wtime() - tc0;

        if (step < size - 1) {
            MPI_Waitall(2, reqs, MPI_STATUSES_IGNORE);
            double *tmp = B_curr;
            B_curr = B_next;
            B_next = tmp;
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    double t1 = MPI_Wtime();

    double local_first = 0.0, local_last = 0.0;
    if (my_rows > 0 && my_row_off == 0) {
        local_first = C_loc[0];
    }
    if (my_rows > 0 && my_row_off + my_rows == n) {
        local_last = C_loc[(size_t)(my_rows - 1) * n + (n - 1)];
    }

    int local_ok = 1;
    double local_err = 0.0;
    if (verify && my_rows > 0) {
        for (int s = 0; s < VERIFY_SAMPLES; s++) {
            int si, sj;
            get_sample_coords(n, s, &si, &sj);
            if (si >= my_row_off && si < my_row_off + my_rows) {
                if (!check_value(si, sj, n, C_loc[(size_t)(si - my_row_off) * n + sj], &local_err)) {
                    local_ok = 0;
                }
            }
        }
    }

    MPI_Reduce(&comp_acc, &st->comp_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_first, &st->c_first, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_last, &st->c_last, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_err, &st->max_err, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_ok, &st->verify_ok, 1, MPI_INT, MPI_LAND, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        st->total_time = t1 - t0;
        st->comm_time = st->total_time - st->comp_time;
    }

    free(A_loc);
    free(C_loc);
    free(B_curr);
    free(B_next);
}

static void run_stream(int n, int rank, int size, const EnvInfo *env, int verify, RunStats *st) {
    int b = env->block_size;
    size_t blk_elems = (size_t)b * b;

    double *A_blk = xmalloc(blk_elems * sizeof(double), "A_blk", rank);
    double *B_blk = xmalloc(blk_elems * sizeof(double), "B_blk", rank);
    double *C_blk = xmalloc(blk_elems * sizeof(double), "C_blk", rank);

    int num_rb = (n + b - 1) / b;
    int num_cb = (n + b - 1) / b;

    MPI_Barrier(MPI_COMM_WORLD);
    double t0 = MPI_Wtime();
    double comp_acc = 0.0;
    double local_first = 0.0, local_last = 0.0;
    double local_err = 0.0;
    int local_ok = 1;

    for (int rb = rank; rb < num_rb; rb += size) {
        int ii = rb * b;
        int mb = (ii + b <= n) ? b : (n - ii);

        for (int cb = 0; cb < num_cb; cb++) {
            int jj = cb * b;
            int nb = (jj + b <= n) ? b : (n - jj);

            for (int kk = 0; kk < n; kk += b) {
                int kb = (kk + b <= n) ? b : (n - kk);

                #pragma omp parallel for schedule(static)
                for (int i = 0; i < mb; i++) {
                    for (int p = 0; p < kb; p++) {
                        A_blk[(size_t)i * kb + p] = elem_a(ii + i, kk + p);
                    }
                }

                #pragma omp parallel for schedule(static)
                for (int p = 0; p < kb; p++) {
                    for (int j = 0; j < nb; j++) {
                        B_blk[(size_t)p * nb + j] = elem_b(kk + p, jj + j);
                    }
                }

                double tc0 = MPI_Wtime();
                dgemm_kernel(A_blk, B_blk, C_blk, mb, nb, kb, nb, kk == 0 ? 0.0 : 1.0);
                comp_acc += MPI_Wtime() - tc0;
            }

            if (ii == 0 && jj == 0) {
                local_first = C_blk[0];
            }
            if (ii + mb == n && jj + nb == n) {
                local_last = C_blk[(size_t)(mb - 1) * nb + (nb - 1)];
            }

            if (verify) {
                for (int s = 0; s < VERIFY_SAMPLES; s++) {
                    int si, sj;
                    get_sample_coords(n, s, &si, &sj);
                    if (si >= ii && si < ii + mb && sj >= jj && sj < jj + nb) {
                        double got = C_blk[(size_t)(si - ii) * nb + (sj - jj)];
                        if (!check_value(si, sj, n, got, &local_err)) {
                            local_ok = 0;
                        }
                    }
                }
            }
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
    double t1 = MPI_Wtime();

    MPI_Reduce(&comp_acc, &st->comp_time, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_first, &st->c_first, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_last, &st->c_last, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_err, &st->max_err, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_ok, &st->verify_ok, 1, MPI_INT, MPI_LAND, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        st->total_time = t1 - t0;
        st->comm_time = st->total_time - st->comp_time;
    }

    free(A_blk);
    free(B_blk);
    free(C_blk);
}

static void execute_matmul(int n, Strategy forced, int plan_only, int verify) {
    int rank = 0, size = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    EnvInfo env = inspect_env(n, rank);
    Strategy strat = select_strategy(n, size, &env, forced, rank);

    RunStats st = {0};
    st.verify_ok = 1;
    st.est_rank_bytes = estimate_rank_bytes(strat, n, size, &env);

    if (plan_only) {
        if (rank == 0) {
            printf("N=%d | ranks=%d (local=%d, threads/rank=%d) | safe_ram=%.1f MB | "
                   "strategy=%s | est_mem/rank=%.1f MB | block=%d | kernel=%s\n",
                   n, size, env.local_size, env.threads,
                   (double)env.min_safe_ram / (1024.0 * 1024.0),
                   strategy_name(strat),
                   (double)st.est_rank_bytes / (1024.0 * 1024.0),
                   env.block_size,
#ifdef USE_OPENBLAS
                   "OpenBLAS"
#else
                   "OpenMP+Tile64"
#endif
            );
            fflush(stdout);
        }
        return;
    }

    switch (strat) {
    case STRAT_LOCAL:  run_local(n, rank, verify, &st); break;
    case STRAT_1D:     run_1d(n, rank, size, verify, &st); break;
    case STRAT_RING:   run_ring(n, rank, size, verify, &st); break;
    case STRAT_STREAM: run_stream(n, rank, size, &env, verify, &st); break;
    default: break;
    }

    if (rank == 0) {
        double flops = 2.0 * (double)n * (double)n * (double)n;
        double gflops_tot = st.total_time > 0.0 ? flops / (st.total_time * 1e9) : 0.0;
        double gflops_cmp = st.comp_time > 0.0 ? flops / (st.comp_time * 1e9) : 0.0;

        printf("N=%d | strategy=%s | ranks=%d (threads/rank=%d) | kernel=%s\n",
               n, strategy_name(strat), size, env.threads,
#ifdef USE_OPENBLAS
               "OpenBLAS"
#else
               "OpenMP+Tile64"
#endif
        );
        printf("mem/rank=%.1f MB (safe=%.1f MB) | total=%.4f s | comp=%.4f s | comm=%.4f s\n",
               (double)st.est_rank_bytes / (1024.0 * 1024.0),
               (double)env.min_safe_ram / (1024.0 * 1024.0),
               st.total_time, st.comp_time, st.comm_time);
        printf("GFLOPS total=%.3f | comp=%.3f | C[0][0]=%.0f | C[N-1][N-1]=%.0f\n",
               gflops_tot, gflops_cmp, st.c_first, st.c_last);
        if (verify) {
            printf("verify=%s (max_err=%.3e)\n", st.verify_ok ? "PASS" : "FAIL", st.max_err);
        }
        fflush(stdout);
    }
}

static Strategy parse_strategy(const char *s, int rank) {
    if (strcmp(s, "auto") == 0)   return STRAT_AUTO;
    if (strcmp(s, "local") == 0)  return STRAT_LOCAL;
    if (strcmp(s, "1d") == 0)     return STRAT_1D;
    if (strcmp(s, "ring") == 0)   return STRAT_RING;
    if (strcmp(s, "stream") == 0) return STRAT_STREAM;
    if (rank == 0) {
        fprintf(stderr, "Unknown strategy '%s' (valid: auto, local, 1d, ring, stream)\n", s);
    }
    MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    return STRAT_AUTO;
}

int main(int argc, char **argv) {
    int provided = 0;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);

    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int n = DEFAULT_N;
    Strategy forced = STRAT_AUTO;
    int plan_only = 0;
    int verify = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--plan-only") == 0) {
            plan_only = 1;
        } else if (strcmp(argv[i], "--no-verify") == 0) {
            verify = 0;
        } else if (strncmp(argv[i], "--strategy=", 11) == 0) {
            forced = parse_strategy(argv[i] + 11, rank);
        } else {
            int val = atoi(argv[i]);
            if (val > 0) {
                n = val;
            } else {
                if (rank == 0) {
                    fprintf(stderr,
                            "Usage: %s [N] [--strategy=auto|local|1d|ring|stream] "
                            "[--plan-only] [--no-verify]\n", argv[0]);
                }
                MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
            }
        }
    }

    execute_matmul(n, forced, plan_only, verify);
    MPI_Finalize();
    return 0;
}
