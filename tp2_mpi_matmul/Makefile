CC      = mpicc
CFLAGS  = -O3 -march=native -Wall -Wextra -std=c11
LDFLAGS = -lm

TARGET  = matmul_mpi
SRC     = src/matmul_mpi.c

.PHONY: all clean run-local

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

run-local: $(TARGET)
	mpirun -np 4 ./$(TARGET) 1024

clean:
	rm -f $(TARGET) slurm-*.out
