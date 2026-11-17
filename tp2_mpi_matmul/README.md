# Multiplicación de Matrices con Números Grandes en MPI (Cronos 2026)

Trabajo práctico de programación paralela distribuida en **C + OpenMPI 5.x** ejecutado mediante **Slurm (PMIx v5)** sobre el clúster **Cronos 2026** (Raspberry Pi 5 · Debian 13 · ARM64).

---

## Estructura del Proyecto

```text
tp2_mpi_matmul/
├── Makefile                        # Compilación optimizada para ARM64 (-O3 -march=native)
├── README.md                       # Documentación técnica y guía de exposición
├── src/
│   └── matmul_mpi.c                # Implementación MPI en C con descomposición por filas
├── slurm/
│   └── matmul_2nodos.sbatch        # Script batch para 2+ nodos en Slurm con PMIx v5
└── scripts/
    └── verify_cronos_stack.sh      # Verificación del stack (GCC, OpenMPI, Python, Slurm)
```

---

## Decisiones de Diseño ("Programación a Conciencia")

### 1. Manejo de Números Grandes (`double` de 64 bits vs `int` de 32 bits)
- Los elementos de $A$ y $B$ se inicializan de forma determinista en el rango $[100\,000,\, 999\,999]$.
- Cada producto parcial $A_{ik} \cdot B_{kj}$ está entre $10^{10}$ y $10^{12}$, lo cual **desbordaría inmediatamente** un entero de 32 bits (`int32`, límite $\approx 2.14 \times 10^9$).
- Se utiliza `double` (IEEE 754 de 64 bits), que representa enteros exactos hasta $2^{53} \approx 9.0 \times 10^{15}$ sin pérdida de precisión para matrices de hasta $N \approx 9000$, además de permitir medir rendimiento estándar en **GFLOPS**.
- Los índices de memoria usan `size_t` (`(size_t)i * n + k`) para evitar desbordamientos de aritmética de punteros en matrices grandes.

### 2. Descomposición de Datos y Colectivas (`MPI_Scatterv` / `MPI_Gatherv` + `MPI_Bcast`)
- **Descomposición 1D por filas:** La matriz $A$ se reparte en bloques horizontales de filas entre los $P$ procesos MPI, mientras que la matriz $B$ se difunde completa a todos los procesos mediante `MPI_Bcast`.
- **Balanceo para cualquier $N$:** En lugar de asumir $N \bmod P = 0$ (que limitaría `MPI_Scatter`), se calculan `sendcounts` y `displs` en `build_row_distribution()` para usar `MPI_Scatterv` y `MPI_Gatherv`. Los primeros $N \bmod P$ rangos procesan $\lfloor N/P \rfloor + 1$ filas y el resto $\lfloor N/P \rfloor$ filas.
- **Memoria contigua 1D:** Las matrices se alojan como arreglos contiguos 1D (`row-major`) en lugar de `double**`, permitiendo enviar bloques enteros en una sola llamada MPI sin fragmentación ni copias extra.

### 3. Optimización de Caché para ARM64 (`i-k-j`)
- El algoritmo clásico `i-j-k` recorre la matriz $B$ por columnas, provocando fallos constantes de caché (*cache misses*) en cada acceso a memoria.
- En `matmul_local_ikj()` se invierten los bucles internos a **`i-k-j`**:
  - `a_ik = a_row[k]` queda en registro.
  - Tanto `b_row[j]` como `c_row[j]` se recorren de forma **contigua y secuencial**, maximizando el aprovechamiento de las líneas de caché L1/L2 del procesador ARM Cortex-A76 de la Raspberry Pi 5 y habilitando vectorización automática (`NEON`) con `-O3 -march=native`.

### 4. Medición de Tiempos y Verificación
- Se sincronizan los procesos con `MPI_Barrier` antes de iniciar `MPI_Wtime()`.
- Se discriminan **Tiempo Total**, **Tiempo de Cómputo** y **Tiempo de Comunicación** (`Bcast + Scatterv + Gatherv`).
- Se calcula el rendimiento en **GFLOPS**: $\frac{2 N^3}{\text{tiempo} \times 10^9}$.
- `verify_samples()` contrasta una muestra distribuida de elementos de $C$ contra el cálculo secuencial exacto en el rango 0 en $O(\text{muestras} \cdot N)$ para validar la correctitud sin penalizar el tiempo total.

---

## Compilación y Ejecución en Cronos 2026

### 1. Verificación previa del stack (opcional)
```bash
chmod +x scripts/verify_cronos_stack.sh
./scripts/verify_cronos_stack.sh
```

### 2. Compilar el programa
```bash
make
```

### 3. Enviar el trabajo por lotes (Slurm, 2 nodos x 4 procesos = 8 MPI ranks)
Ejecutar desde la carpeta `tp2_mpi_matmul/` (en el filesystem compartido entre los nodos):
```bash
sbatch slurm/matmul_2nodos.sbatch
```
O especificando un tamaño de matriz distinto (por ejemplo `4096`):
```bash
sbatch slurm/matmul_2nodos.sbatch 4096
```

### 4. Monitorear y revisar resultados
```bash
squeue -u $USER
cat logs/matmul_<JOB_ID>.out
```

---

## Guía Rápida para Explicar el TP

1. **Arquitectura:** El nodo `caronte` recibe el script `sbatch` y Slurm asigna 2 o más nodos de cómputo (`nodo0`–`nodo7`), lanzando 4 procesos MPI por nodo (1 por núcleo de la Raspberry Pi 5) coordinados vía **PMIx v5**.
2. **Flujo MPI:**
   - Rango 0 inicializa $A$ y $B$.
   - `MPI_Bcast` envía $B$ a todos los rangos.
   - `MPI_Scatterv` reparte las filas de $A$ equitativamente (incluso si $N$ no es múltiplo de $P$).
   - Cada rango calcula sus filas de $C$ en paralelo usando bucles `i-k-j` amigables con caché.
   - `MPI_Gatherv` recolecta el resultado completo en el rango 0, que imprime métricas y verifica exactitud.
3. **Por qué escala y por qué `double`:** El cómputo crece como $O(N^3)$ mientras la comunicación crece como $O(N^2)$, por lo que al aumentar $N$ el costo de red se amortiza frente al cómputo paralelo. Usar `double` evita el desbordamiento de `int32` al multiplicar números grandes.
