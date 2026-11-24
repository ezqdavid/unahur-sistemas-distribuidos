CC      = mpicc
CFLAGS  = -O3 -march=native -fopenmp -Wall -Wextra -std=c11
LDFLAGS = -lm

USE_OPENBLAS ?= $(shell (pkg-config --exists openblas 2>/dev/null || [ -f /usr/include/cblas.h ] || [ -f /usr/include/x86_64-linux-gnu/cblas.h ] || [ -f /usr/include/aarch64-linux-gnu/cblas.h ]) && echo 1 || echo 0)

ifeq ($(USE_OPENBLAS),1)
    CFLAGS  += -DUSE_OPENBLAS $(shell pkg-config --cflags openblas 2>/dev/null)
    LDFLAGS += $(shell pkg-config --libs openblas 2>/dev/null || echo -lopenblas)
endif

TARGET = matmul_mpi
SRC    = src/matmul_mpi.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET) slurm-*.out slurm-*.err
