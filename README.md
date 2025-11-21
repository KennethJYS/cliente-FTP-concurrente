# Cliente FTP Concurrente

Cliente FTP completo implementado en C con soporte para transferencias concurrentes mediante procesos.

## 👤 Autor
**[Kenneth Yar]**  
Computación Distribuida - [21-11-2025]

## 📋 Descripción

Cliente FTP que implementa el protocolo RFC 959 con capacidad de realizar múltiples transferencias de archivos de manera concurrente, manteniendo activa la conexión de control.

## ✨ Características Implementadas

### Comandos Básicos (RFC 959)
- **USER/PASS**: Autenticación con el servidor FTP
- **RETR** (`get`): Descarga de archivos en modo PASV
- **STOR** (`put`): Carga de archivos en modo PASV  
- **STOR** (`pput`): Carga de archivos en modo PORT (activo)
- **LIST** (`dir`): Listado de directorio en modo PASV
- **QUIT**: Cierre de sesión

### Comandos Adicionales (Extra Crédito)
- **PWD** (`pwd`): Muestra directorio de trabajo actual
- **CWD** (`cd`): Cambia de directorio
- **MKD** (`mkd`): Crea nuevo directorio
- **DELE** (`dele`): Elimina archivo del servidor

### Concurrencia
- Utiliza `fork()` para crear procesos hijo
- Permite múltiples transferencias simultáneas (GET/PUT)
- Manejo correcto de señal SIGCHLD (evita procesos zombie)
- Conexión de control permanece responsiva durante transferencias

## 🔧 Compilación

```bash
make
```

Para limpiar archivos objeto y ejecutable:
```bash
make clean
```

## 🚀 Uso

### Conectar al Servidor
```bash
./clienteFTP <host> [puerto]
```

Ejemplos:
```bash
./clienteFTP localhost
./clienteFTP ftp.example.com
./clienteFTP 192.168.1.100 2121
```

### Sesión de Ejemplo
```
$ ./clienteFTP localhost
220 (vsFTPd 3.0.5)
Please enter your username: testuser
Enter your password: 
230 Login successful.

ftp> help
Cliente FTP Concurrente. Comandos:
 help           - muestra esta ayuda
 dir            - LIST (modo PASV)
 get <archivo>  - RETR en PASV (concurrente)
 put <archivo>  - STOR en PASV (concurrente)
 pput <archivo> - STOR en PORT (modo activo, concurrente)
 cd <dir>       - CWD
 pwd            - PWD (extra)
 mkd <dir>      - MKD (extra)
 dele <file>    - DELE (extra)
 quit           - QUIT

ftp> pwd
257 "/" is current directory.

ftp> dir
-rw-r--r--    1 1000     1000         1024 Nov 20 10:30 file1.txt
-rw-r--r--    1 1000     1000         2048 Nov 20 10:31 file2.txt
226 Directory send OK.

ftp> get file1.txt
150 Opening BINARY mode data connection.
Transferencia GET iniciada (PID 12345)
226 Transfer complete.

ftp> put documento.pdf
150 Ok to send data.
Transferencia PUT iniciada (PID 12346)
226 Transfer complete.

ftp> pput archivo.bin
200 PORT command successful.
150 Ok to send data.
Transferencia PPUT iniciada (PID 12347)
226 Transfer complete.

ftp> quit
221 Goodbye.
```

## 🧪 Pruebas

### Pruebas Manuales Recomendadas

**1. Comandos básicos:**
```
ftp> help
ftp> pwd
ftp> dir
```

**2. Descarga de archivo:**
```
ftp> get archivo.txt
```

**3. Subida de archivo (PASV):**
```
ftp> put local.txt
```

**4. Subida de archivo (PORT):**
```
ftp> pput documento.pdf
```

**5. Comandos de directorio:**
```
ftp> mkd nuevodirectorio
ftp> cd nuevodirectorio
ftp> pwd
ftp> cd ..
ftp> dele archivo_temporal.txt
```

**6. Concurrencia:**
```
# Ejecutar múltiples comandos get/put rápidamente
ftp> get archivo1.txt
ftp> get archivo2.txt
ftp> get archivo3.txt
ftp> dir  # Debe responder mientras las transferencias continúan
```

## 📁 Estructura del Proyecto

```
YarK-clienteFTP/
├── YarK-clienteFTP.c    # Código principal del cliente
├── connectsock.c        # Funciones de conexión de sockets
├── connectTCP.c         # Conexión TCP
├── passivesock.c        # Modo pasivo
├── passiveTCP.c         # TCP pasivo
├── errexit.c            # Manejo de errores
├── Makefile             # Script de compilación
└── README.md            # Este archivo
```

## 📊 Requisitos Cumplidos

✅ Usa funciones `connectsock.c`, `connectTCP.c`, `errexit.c`  
✅ Implementa comandos básicos: USER, PASS, STOR, RETR, PORT, PASV  
✅ Implementa comandos extra: PWD, MKD, CWD, DELE  
✅ Transferencias concurrentes con conexión de control activa  
✅ Implementación con procesos (`fork()`)