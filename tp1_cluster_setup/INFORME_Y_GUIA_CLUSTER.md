# Práctica: Instalación y Configuración de un Clúster Raspberry Pi
**Materia:** Sistemas Distribuidos — Universidad Nacional de Hurlingham (UNAHUR)  
**Fecha:** 27 de Octubre de 2026  
**Integrantes del Grupo:**
- [Nombre y Apellido Estudiante 1]
- [Nombre y Apellido Estudiante 2]
- Ezequiel [Apellido]

---

## 1. Resumen General

El presente trabajo práctico aborda el diseño, interconexión, configuración y puesta en marcha de un clúster de computación distribuida basado en microcomputadoras Raspberry Pi. El objetivo central consiste en construir una infraestructura funcional de Alto Rendimiento (HPC) capaz de gestionar recursos heterogéneos, planificar trabajos en paralelo y brindar un sistema de archivos compartido y consistente a través de la red de área local.

La arquitectura se divide en:
* **Nodo Maestro (`nodo0`):** Cumple los roles de pasarela de acceso (Login Node), servidor de almacenamiento de red distribuido (NFS Server) y controlador central del planificador de tareas (Slurm Controller Daemon `slurmctld`).
* **Nodos Trabajadores / Esclavos (`nodo1`, `nodo2`, `nodo3`):** Nodos dedicados exclusivamente a la ejecución de procesos y cómputo de trabajos mediante el agente `slurmd`, consumiendo de forma transparente los datos binarios montados desde el nodo maestro.
* **Red Aislada de Alta Velocidad:** Interconexión mediante Switch Gigabit y un Router Linksys E900 bajo direccionamiento estático en la subred `192.168.1.0/24`, con aislamiento perimetral mediante NAT hacia la red institucional (WAN).

---

## 2. Topología de Red y Asignación de Direccionamiento

| Dispositivo / Rol | Hostname | IP Asignada | Máscara | Puerta de Enlace (Gateway) | Servidores DNS |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Router Linksys E900** | — | `192.168.1.254` | `255.255.255.0` (`/24`) | Asignada por WAN UNAHUR | ISP / `8.8.8.8` |
| **Nodo 0 (Master / Login)** | `nodo0` | `192.168.1.190` | `255.255.255.0` (`/24`) | `192.168.1.254` | `192.168.1.254`, `8.8.8.8` |
| **Nodo 1 (Worker)** | `nodo1` | `192.168.1.191` | `255.255.255.0` (`/24`) | `192.168.1.254` | `192.168.1.254`, `8.8.8.8` |
| **Nodo 2 (Worker)** | `nodo2` | `192.168.1.192` | `255.255.255.0` (`/24`) | `192.168.1.254` | `192.168.1.254`, `8.8.8.8` |
| **Nodo 3 (Worker)** | `nodo3` | `192.168.1.193` | `255.255.255.0` (`/24`) | `192.168.1.254` | `192.168.1.254`, `8.8.8.8` |

* **SSID Wi-Fi de administración:** `WIFICluster`
* **Clave de red Wi-Fi:** `cluster1101`
* **Usuario por defecto:** `usuario`
* **Contraseña por defecto:** `pepita`

---

## 3. Checklist General de Progreso

### Fase 1: Preparación Inicial y Hardware
- [ ] Flasheo de microSD con Raspberry Pi OS Lite (64-bit) para todos los nodos.
- [ ] Habilitación de servicio SSH y seteo de credenciales (`usuario:pepita`).
- [ ] Conexión física al Switch Gigabit Ethernet y encendido de placas.

### Fase 2: Configuración del Sistema Base y Red
- [ ] **Configurar cgroups en kernel (`cmdline.txt`)** en todos los nodos:
  - `cgroup_enable=memory cgroup_memory=1 swapaccount=1`
- [ ] Reinicio de validación: `sudo reboot`
- [ ] Deshabilitar sobreescritura de cloud-init en `/etc/cloud/cloud.cfg` (`- update_etc_hosts`).
- [ ] Configuración de `/etc/hosts` con resolución de todos los nodos.
- [ ] Asignación de IP fija mediante `sudo nmtui` en interfaz cableada `eth0`.
- [ ] Sincronización horaria con `ntpsec-ntpdate`.

### Fase 3: Almacenamiento Compartido Distribuido (NFS)
- [ ] Conectar pendrive USB al Nodo Maestro (`nodo0`).
- [ ] Identificar dispositivo (`lsblk`) y formatear partición (`mkfs.ext4 /dev/sda1`).
- [ ] Creación de punto de montaje `/clusterfs` y asignación de permisos (`chmod 777`).
- [ ] Configuración de montaje automático en `/etc/fstab` mediante UUID.
- [ ] Instalación y configuración de `nfs-kernel-server` en `nodo0` (`/etc/exports`).
- [ ] Instalación de `nfs-common` y montaje de `/clusterfs` en cada Nodo Worker (`nodo1`, `nodo2`, `nodo3`).

### Fase 4: Autenticación y Planificador de Recursos (Munge y Slurm)
- [ ] Instalación de Munge en todos los nodos.
- [ ] Generación de la clave criptográfica `munge.key` en `nodo0` y distribución a los workers con permisos estrictos (`400`).
- [ ] Inicio de servicio Munge (`systemctl enable --now munge`).
- [ ] Instalación de `slurm-wlm` en `nodo0` y `slurmd` + `slurm-client` en workers.
- [ ] Configuración sincronizada de `slurm.conf`, `cgroup.conf` y `cgroup_allowed_devices_file.conf`.
- [ ] Inicio de demonio Slurm Controller (`slurmctld`) en Master y `slurmd` en todos los nodos.

### Fase 5: Pruebas de Cómputo Distribuido y Validación
- [ ] Validación de conectividad y ping cruzado sin pérdida de paquetes.
- [ ] Autenticación SSH sin clave entre nodos (claves públicas `id_ed25519`).
- [ ] Test de lectura/escritura concurrente en `/clusterfs`.
- [ ] Verificación de Munge (`munge -n | unmunge`).
- [ ] Verificación del estado del clúster con `sinfo -N -l` (todos los nodos en estado `idle`).
- [ ] Ejecución de trabajo interactivo paralelo distribuido: `srun --nodes=4 hostname`.

---

## 4. Guía Detallada Paso a Paso con Comandos Explicados

### 4.1. Configuración de Kernel y Grupos de Control (Cgroups)
En sistemas de cómputo compartido con memoria RAM limitada (como las Raspberry Pi 3/4), el planificador Slurm requiere de forma mandatoria que el kernel de Linux contabilice y limite estrictamente el uso de memoria de cada proceso.

1. Editar los parámetros de inicio del kernel:
   ```bash
   sudo nano /boot/firmware/cmdline.txt
   ```
2. Al final de la **única línea existente**, agregar un espacio y colocar:
   ```text
   cgroup_enable=memory cgroup_memory=1 swapaccount=1
   ```
   > *Nota crítica:* NO presionar Enter ni crear una segunda línea; todo debe ser una sola línea de comandos para el cargador de arranque.
3. Reiniciar el nodo:
   ```bash
   sudo reboot
   ```

---

### 4.2. Resolución de Nombres Local y Configuración de Red

1. **Evitar que el sistema sobreescriba `/etc/hosts`:**
   ```bash
   sudo nano /etc/cloud/cloud.cfg
   ```
   Buscar y comentar con un `#` la línea:
   ```yaml
   # - update_etc_hosts
   ```

2. **Configurar el archivo `/etc/hosts` (idéntico en todos los nodos):**
   ```bash
   sudo nano /etc/hosts
   ```
   Asegurar que contenga las siguientes entradas:
   ```text
   127.0.0.1       localhost
   192.168.1.190   nodo0
   192.168.1.191   nodo1
   192.168.1.192   nodo2
   192.168.1.193   nodo3
   ```

3. **Fijar IP estática con NetworkManager Text UI:**
   ```bash
   sudo nmtui
   ```
   - Ir a *"Editar una conexión" (Edit a connection)* -> seleccionar `eth0` (cableada).
   - En IPv4 cambiar de `<Automático>` a `<Manual>` -> seleccionar `<Mostrar>`.
   - Completar según el nodo correspondiente (ejemplo para `nodo0`):
     - **Direcciones:** `192.168.1.190/24`
     - **Puerta de enlace:** `192.168.1.254`
     - **Servidores DNS:** `192.168.1.254, 8.8.8.8`
   - Guardar y salir.
   - Reiniciar el servicio de red:
     ```bash
     sudo systemctl restart NetworkManager
     ```

4. **Sincronización de Tiempo (NTP):**
   ```bash
   sudo apt update
   sudo apt install ntpsec-ntpdate -y
   sudo ntpdate 192.168.1.254 || sudo ntpdate pool.ntp.org
   ```

---

### 4.3. Servidor de Almacenamiento Compartido (NFS en `nodo0`)

1. **Preparar y formatear la unidad de almacenamiento (Pendrive USB):**
   ```bash
   lsblk
   # Identificar la partición (ejemplo: /dev/sda1)
   sudo mkfs.ext4 /dev/sda1
   ```

2. **Crear el directorio y establecer permisos:**
   ```bash
   sudo mkdir -p /clusterfs
   sudo chown nobody:nogroup -R /clusterfs
   sudo chmod 777 -R /clusterfs
   ```

3. **Configurar montaje permanente en `/etc/fstab`:**
   ```bash
   sudo blkid /dev/sda1
   # Copiar el UUID obtenido (ej: UUID=a1b2c3d4-...)
   sudo nano /etc/fstab
   ```
   Agregar al final del archivo:
   ```text
   UUID=<TU_UUID_AQUI>    /clusterfs    ext4    defaults    0    2
   ```
   Aplicar cambios y recargar systemd:
   ```bash
   sudo systemctl daemon-reload
   sudo mount -a
   findmnt /clusterfs
   ```

4. **Exportar la carpeta mediante NFS Server:**
   ```bash
   sudo apt install nfs-kernel-server -y
   sudo nano /etc/exports
   ```
   Agregar la directiva de red local:
   ```text
   /clusterfs 192.168.1.0/24(rw,sync,no_root_squash,no_subtree_check)
   ```
   Aplicar la exportación:
   ```bash
   sudo exportfs -ra
   sudo systemctl restart nfs-kernel-server
   ```

---

### 4.4. Cliente NFS en Nodos Workers (`nodo1`, `nodo2`, `nodo3`)

1. **Instalar dependencias y preparar punto de montaje:**
   ```bash
   sudo apt update
   sudo apt install nfs-common -y
   sudo mkdir -p /clusterfs
   ```

2. **Configurar montaje permanente en `/etc/fstab`:**
   ```bash
   sudo nano /etc/fstab
   ```
   Agregar la siguiente línea:
   ```text
   192.168.1.190:/clusterfs    /clusterfs    nfs    defaults    0    0
   ```
3. **Montar y verificar:**
   ```bash
   sudo mount -a
   findmnt /clusterfs
   ```

---

### 4.5. Configuración de Seguridad Criptográfica (Munge)
Munge es el servicio de autenticación encargado de generar y validar credenciales de identidad cifradas entre los nodos del clúster sin requerir contraseñas en cada solicitud de Slurm.

1. **Instalar en todos los nodos:**
   ```bash
   sudo apt install munge libmunge-dev -y
   ```

2. **En el Nodo Maestro (`nodo0`):**
   Generar una clave segura si no existe y copiarla al almacenamiento compartido `/clusterfs`:
   ```bash
   sudo cp /etc/munge/munge.key /clusterfs/
   sudo chown munge:munge /etc/munge/munge.key
   sudo chmod 400 /etc/munge/munge.key
   sudo systemctl enable --now munge
   ```

3. **En cada Nodo Worker (`nodo1`, `nodo2`, `nodo3`):**
   Copiar la clave idéntica desde `/clusterfs` y ajustar permisos estrictos:
   ```bash
   sudo cp /clusterfs/munge.key /etc/munge/munge.key
   sudo chown munge:munge /etc/munge/munge.key
   sudo chmod 400 /etc/munge/munge.key
   sudo systemctl enable --now munge
   ```

4. **Validar funcionamiento:**
   ```bash
   munge -n | unmunge
   ```
   *(Debe retornar `STATUS: Success` con la información del socket de autenticación).*

---

### 4.6. Configuración del Planificador de Recursos (Slurm)

1. **Instalación de paquetes:**
   * En `nodo0` (Master):
     ```bash
     sudo apt install slurm-wlm -y
     ```
   * En Workers (`nodo1`, `nodo2`, `nodo3`):
     ```bash
     sudo apt install slurmd slurm-client -y
     ```

2. **Sincronización de configuraciones (`slurm.conf` y `cgroups`):**
   Copiar desde el docente los archivos provistos a `/clusterfs` para que todos los nodos compartan exactamente el mismo hash de configuración:
   * `/etc/slurm/slurm.conf`
   * `/etc/slurm/cgroup.conf`
   * `/etc/slurm/cgroup_allowed_devices_file.conf`

   En cada nodo worker:
   ```bash
   sudo cp /clusterfs/slurm.conf /etc/slurm/
   sudo cp /clusterfs/cgroup* /etc/slurm/
   ```

3. **Iniciar servicios:**
   * En `nodo0` (Master):
     ```bash
     sudo systemctl enable --now slurmctld
     sudo systemctl enable --now slurmd
     ```
   * En Workers:
     ```bash
     sudo systemctl enable --now slurmd
     ```

---

## 5. Pruebas de Validación y Salidas de Consola

### 5.1. Prueba de Estado General del Clúster (`sinfo`)
```bash
sinfo -N -l
```
*Salida esperada:*
```text
NODELIST   NODES PARTITION       STATE CPUS    S:C:T MEMORY TMP_DISK WEIGHT AVAIL_FE REASON
nodo0          1    debug*        idle    4    1:4:1    900        0      1   (null) none
nodo1          1    debug*        idle    4    1:4:1    900        0      1   (null) none
nodo2          1    debug*        idle    4    1:4:1    900        0      1   (null) none
nodo3          1    debug*        idle    4    1:4:1    900        0      1   (null) none
```

### 5.2. Prueba de Ejecución Paralela Distribuida (`srun`)
```bash
srun --nodes=4 --ntasks=4 hostname
```
*Salida esperada:*
```text
nodo0
nodo1
nodo2
nodo3
```

---

## 6. Registro de Problemas Encontrados y Soluciones Aplicadas (Troubleshooting)

### Caso 1: Discrepancia en la IP de la Puerta de Enlace (`.154` vs `.254`)
* **Problema:** Se reportó duda en el laboratorio sobre si la puerta de enlace era `192.168.1.154` o `192.168.1.254`.
* **Diagnóstico técnico:** Se efectuó un barrido de ICMP (ping) y consulta al servidor DHCP del Linksys E900. Se confirmó que `192.168.1.154` no responde (`Host de destino inaccesible`), mientras que `192.168.1.254` respondió en 11 ms y fue entregada como router oficial por DHCP.
* **Solución aplicada:** Se ratificó en todos los nodos que el Gateway debe ser obligatoriamente `192.168.1.254`.

### Caso 2: Pérdida de conectividad al Wi-Fi del Clúster (Aislamiento de Red)
* **Problema:** Al conectarse a la red `WIFICluster`, la máquina pierde salida a Internet si el router Linksys no tiene conectada la WAN universitaria, impidiendo consultar documentación y herramientas de soporte.
* **Solución aplicada:** Se implementó una arquitectura de doble interfaz (*Dual-Homing*) en la notebook de administración:
  1. Conexión a Internet mediante *Tethering* USB del teléfono móvil (`Ethernet 2`, métrica de ruta prioritaria 25).
  2. Conexión de red local a `WIFICluster` mediante la placa Wi-Fi (`192.168.1.45`, métrica 35).
  Esto permitió mantener comunicación SSH con el clúster (`192.168.1.190`) sin interrumpir la salida a Internet.

### Caso 3: Fallo de Munge por permisos de archivo de clave
* **Problema:** El demonio `munge` entra en estado `failed` al iniciar.
* **Causa:** La especificación de seguridad de Munge prohíbe que `munge.key` sea legible por usuarios distintos al usuario de sistema `munge`.
* **Solución:** Ejecutar `sudo chown munge:munge /etc/munge/munge.key` y `sudo chmod 400 /etc/munge/munge.key`.

---

## 7. Conclusiones y Aprendizajes
[Espacio para completar por el grupo con las conclusiones finales previas a la entrega]
