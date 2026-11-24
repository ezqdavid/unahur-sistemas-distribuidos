FROM debian:trixie-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    openmpi-bin \
    libopenmpi-dev \
    libopenblas-dev \
    openssh-server \
    openssh-client \
    && rm -rf /var/lib/apt/lists/*

# Configurar SSH sin contraseña entre contenedores para simular nodos MPI
RUN mkdir -p /var/run/sshd /root/.ssh \
    && ssh-keygen -t ed25519 -f /root/.ssh/id_ed25519 -N "" \
    && cp /root/.ssh/id_ed25519.pub /root/.ssh/authorized_keys \
    && printf "Host *\n  StrictHostKeyChecking no\n  UserKnownHostsFile /dev/null\n  LogLevel ERROR\n" > /root/.ssh/config \
    && chmod 700 /root/.ssh \
    && chmod 600 /root/.ssh/authorized_keys /root/.ssh/config

WORKDIR /workspace
CMD ["/usr/sbin/sshd", "-D"]
