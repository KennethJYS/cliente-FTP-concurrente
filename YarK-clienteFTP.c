/* YarK-clienteFTP.c - Cliente FTP Concurrente */

#define _POSIX_C_SOURCE 200809L
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdarg.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <ctype.h>

extern int  errno;

/* Prototipos de funciones externas (de tus archivos) */
int  errexit(const char *format, ...);
int  connectTCP(const char *host, const char *service);
int  passiveTCP(const char *service, int qlen);

/* Config */
#define LINELEN 512
#define QLEN 5
int s_control;
char g_host[128] = "localhost";

/* ---------------- utilidades de lectura/envío (robustas) ---------------- */

/* read_line: lee hasta '\n' (incluye '\n' en el buffer). Devuelve bytes leídos o -1/0 */
ssize_t read_line(int fd, char *buf, size_t max) {
    size_t n = 0;
    while (n < max - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r == 0) { /* EOF */
            if (n == 0) return 0;
            break;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf[n++] = c;
        if (c == '\n') break;
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

/* expect_reply: lee reply completo del canal de control (incluye multilínea RFC959).
 * out debe tener tamaño outsz. Devuelve código numérico (p.ej. 220) o -1 en error.
 */
int expect_reply(int ctrl_sock, char *out, size_t outsz) {
    char line[LINELEN];
    out[0] = '\0';
    int code = 0;
    int first = 1;
    char code_str[4] = {0};

    while (1) {
        ssize_t r = read_line(ctrl_sock, line, sizeof(line));
        if (r <= 0) return -1;
        /* concatenar con seguridad */
        if (strlen(out) + strlen(line) + 1 < outsz) strncat(out, line, outsz - strlen(out) - 1);

        /* detección de multiline */
        if (first) {
            if (strlen(line) >= 3 && isdigit((unsigned char)line[0]) &&
                isdigit((unsigned char)line[1]) && isdigit((unsigned char)line[2])) {
                code_str[0] = line[0]; code_str[1] = line[1]; code_str[2] = line[2]; code_str[3] = '\0';
                code = (code_str[0]-'0')*100 + (code_str[1]-'0')*10 + (code_str[2]-'0');
                first = 0;
                if (strlen(line) >= 4 && line[3] == '-') {
                    /* multiline, seguir leyendo */
                    continue;
                } else {
                    /* single-line reply -> terminado */
                    break;
                }
            } else {
                /* respuesta mal formada, continuar leyendo hasta newline */
            }
        } else {
            if (strlen(line) >= 4 && strncmp(line, code_str, 3) == 0 && line[3] == ' ') {
                /* fin de multiline */
                break;
            }
        }
    }
    return code;
}

/* send_cmd: enviar comando y leer reply completo. Rellenar out con reply.
 * Devuelve código numérico o -1 en error.
 */
int send_cmd(int sock, char *out, size_t outsz, const char *fmt, ...) {
    char cmd[LINELEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd) - 3, fmt, ap);
    va_end(ap);
    /* asegurar CRLF */
    if (strlen(cmd) + 3 < sizeof(cmd)) strncat(cmd, "\r\n", sizeof(cmd) - strlen(cmd) - 1);

    size_t tosend = strlen(cmd);
    size_t sent = 0;
    while (sent < tosend) {
        ssize_t s = send(sock, cmd + sent, tosend - sent, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return -1;
        }
        sent += (size_t)s;
    }

    /* leer reply */
    int code = expect_reply(sock, out, outsz);
    return code;
}

/* ---------------- PASV y PORT robustos ---------------- */

/* pasivo_conn: pide PASV, parsea respuesta, y conecta al host:port devuelto.
 * Devuelve socket de datos conectado o -1 en error.
 */
int pasivo_conn(int ctrl_sock) {
    char reply[LINELEN];
    int code = send_cmd(ctrl_sock, reply, sizeof(reply), "PASV");
    if (code < 0) return -1;
    if (code / 100 != 2) {
        fprintf(stderr, "PASV failed: %s\n", reply);
        return -1;
    }
    char *p = strchr(reply, '(');
    if (!p) {
        fprintf(stderr, "PASV reply malformed: %s\n", reply);
        return -1;
    }
    int h1,h2,h3,h4,p1,p2;
    if (sscanf(p+1, "%d,%d,%d,%d,%d,%d", &h1,&h2,&h3,&h4,&p1,&p2) != 6) {
        fprintf(stderr, "PASV parse failed on: %s\n", p+1);
        return -1;
    }
    char host[64];
    int port = p1*256 + p2;
    snprintf(host, sizeof(host), "%d.%d.%d.%d", h1,h2,h3,h4);
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    int sdata = connectTCP(host, portstr);
    if (sdata < 0) {
        perror("connect (PASV)");
        return -1;
    }
    return sdata;
}

/* configurar_port: crea socket de escucha efímero y forma comando PORT correcto
 * usando la IP local de la conexión de control (getsockname sobre s_control).
 * s_listen será el socket de escucha; port_cmd se rellena con "PORT a,b,c,d,p1,p2".
 * Devuelve 0 éxito o -1 error.
 */
int configurar_port(int *s_listen, char *port_cmd, size_t port_cmd_sz) {
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    int s, opt = 1;

    /* Crear socket manualmente en lugar de usar passiveTCP */
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return -1;
    }

    /* Permitir reusar la dirección */
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(s);
        return -1;
    }

    /* Bind a puerto 0 (puerto efímero asignado por el SO) */
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = 0;  /* Puerto 0 = puerto efímero */

    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind");
        close(s);
        return -1;
    }

    /* Listen */
    if (listen(s, QLEN) < 0) {
        perror("listen");
        close(s);
        return -1;
    }

    *s_listen = s;

    /* Obtener el puerto asignado */
    len = sizeof(sin);
    if (getsockname(s, (struct sockaddr *)&sin, &len) < 0) {
        perror("getsockname(listen)");
        close(s);
        return -1;
    }
    int port_num = ntohs(sin.sin_port);
    int p1 = port_num / 256;
    int p2 = port_num % 256;

    /* Obtener IP local usada en s_control */
    struct sockaddr_in localaddr;
    socklen_t locallen = sizeof(localaddr);
    if (getsockname(s_control, (struct sockaddr*)&localaddr, &locallen) < 0) {
        perror("getsockname(control)");
        close(s);
        return -1;
    }
    unsigned char *ip = (unsigned char *)&localaddr.sin_addr.s_addr;
    
    /* Formar comando PORT */
    snprintf(port_cmd, port_cmd_sz, "PORT %u,%u,%u,%u,%d,%d",
             ip[0], ip[1], ip[2], ip[3], p1, p2);
    
    return 0;
}
/* ---------------- manejo de Señales ---------------- */
void reaper(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}

/* ---------------- Ayuda ---------------- */
void ayuda() {
    printf("Cliente FTP Concurrente. Comandos:\n"
           " help           - muestra esta ayuda\n"
           " dir            - LIST (modo PASV)\n"
           " get <archivo>  - RETR en PASV (concurrente)\n"
           " put <archivo>  - STOR en PASV (concurrente)\n"
           " pput <archivo> - STOR en PORT (modo activo, concurrente)\n"
           " cd <dir>       - CWD\n"
           " pwd            - PWD (extra)\n"
           " mkd <dir>      - MKD (extra)\n"
           " dele <file>    - DELE (extra)\n"
           " quit           - QUIT\n");
}

char* read_password(const char *prompt) {
    static char password[128];
    struct termios old, new;
    
    printf("%s", prompt);
    fflush(stdout);
    
    // Deshabilitar echo
    if (tcgetattr(STDIN_FILENO, &old) != 0) {
        perror("tcgetattr");
        return NULL;
    }
    new = old;
    new.c_lflag &= ~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &new) != 0) {
        perror("tcsetattr");
        return NULL;
    }
    
    if (fgets(password, sizeof(password), stdin) == NULL) {
        password[0] = '\0';
    }
    password[strcspn(password, "\r\n")] = '\0';
    
    // Restaurar echo
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    printf("\n");
    
    return password;
}

/* ---------------- Main ---------------- */
int main(int argc, char *argv[]) {
    char reply[LINELEN], data_buf[LINELEN];
    char prompt[256];
    char user[64];
    char *arg;
    int sdata, n;
    FILE *fp;
    pid_t pid;

    /* manejo de argumentos: host [port] */
    if (argc >= 2) {
        strncpy(g_host, argv[1], sizeof(g_host)-1);
        g_host[sizeof(g_host)-1] = '\0';
    }
    const char *service = (argc >= 3) ? argv[2] : "ftp";

    /* instalar handler SIGCHLD */
    struct sigaction sa;
    sa.sa_handler = reaper;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        /* no fatal, continuar */
    }

    /* conectar control */
    s_control = connectTCP(g_host, service);
    if (s_control < 0) errexit("No pudo conectar a %s:%s\n", g_host, service);

    /* leer greeting */
    if (expect_reply(s_control, reply, sizeof(reply)) < 0) {
        close(s_control);
        errexit("Error leyendo greeting del servidor\n");
    }
    printf("%s", reply);

    /* login interactivo */
    while (1) {
        printf("Please enter your username: ");
        if (fgets(user, sizeof(user), stdin) == NULL) exit(1);
        user[strcspn(user, "\r\n")] = '\0';
        int code = send_cmd(s_control, reply, sizeof(reply), "USER %s", user);
        if (code < 0) { close(s_control); errexit("Error en USER\n"); }
        printf("%s", reply);

        char *pass = read_password("Enter your password: ");
        code = send_cmd(s_control, reply, sizeof(reply), "PASS %s", pass ? pass : "");
        if (code < 0) { close(s_control); errexit("Error en PASS\n"); }
        printf("%s", reply);

        if (code == 230) break; /* login correcto */
        /* si 3xx -> continuar, 5xx -> retry */
    }

    /* asegurar modo binario para transferencias */
    if (send_cmd(s_control, reply, sizeof(reply), "TYPE I") < 0) {
        fprintf(stderr, "Aviso: no se pudo cambiar a TYPE I\n");
    } else {
        /* show result */
        // printf("%s", reply);
    }

    ayuda();

    /* bucle principal */
    while (1) {
        printf("ftp> ");
        if (fgets(prompt, sizeof(prompt), stdin) == NULL) break;
        prompt[strcspn(prompt, "\r\n")] = '\0';
        if (prompt[0] == '\0') continue;

        char *ucmd = strtok(prompt, " ");
        arg = strtok(NULL, " ");

        if (strcmp(ucmd, "help") == 0) {
            ayuda();
            continue;
        }

        if (strcmp(ucmd, "quit") == 0) {
            if (send_cmd(s_control, reply, sizeof(reply), "QUIT") >= 0) {
                printf("%s", reply);
            }
            break;
        }

        if (strcmp(ucmd, "dir") == 0) {
            sdata = pasivo_conn(s_control);
            if (sdata < 0) { fprintf(stderr, "No se pudo abrir PASV\n"); continue; }
            if (send_cmd(s_control, reply, sizeof(reply), "LIST") < 0) { close(sdata); continue; }
            /* leer datos y mostrar */
            while ((n = recv(sdata, data_buf, sizeof(data_buf), 0)) > 0) {
                fwrite(data_buf, 1, n, stdout);
            }
            close(sdata);
            if (expect_reply(s_control, reply, sizeof(reply)) >= 0) {
                printf("%s", reply);
            }
            continue;
        }

        if (strcmp(ucmd, "get") == 0) {
            if (!arg) { printf("Uso: get <archivo>\n"); continue; }
            
            sdata = pasivo_conn(s_control);
            if (sdata < 0) continue;
            
            if (send_cmd(s_control, reply, sizeof(reply), "RETR %s", arg) < 0) { 
                close(sdata); 
                continue; 
            }
            
            if (reply[0] != '1') { /* no 1xx -> error */
                printf("%s", reply);
                close(sdata);
                continue;
            }
            
            pid = fork();
            if (pid < 0) { 
                perror("fork"); 
                close(sdata); 
                continue; 
            }
            
            if (pid == 0) {
                /* HIJO: recibe y escribe archivo */
                FILE *fp_child = fopen(arg, "wb");
                if (!fp_child) { 
                    perror("fopen"); 
                    close(sdata); 
                    exit(1); 
                }
                
                while ((n = recv(sdata, data_buf, sizeof(data_buf), 0)) > 0) {
                    fwrite(data_buf, 1, n, fp_child);
                }
                
                fclose(fp_child);
                close(sdata);
                
                /* Hijo NO lee del socket de control - el padre lo hará */
                exit(0);
            } else {
                /* PADRE: cierra socket de datos y lee respuesta final */
                close(sdata);
                printf("Transferencia GET iniciada (PID %d)\n", pid);
                
                /* Leer respuesta final 226 */
                if (expect_reply(s_control, reply, sizeof(reply)) >= 0) {
                    printf("%s", reply);
                }
                continue;
            }
        }

        if (strcmp(ucmd, "put") == 0) {
            if (!arg) { printf("Uso: put <archivo>\n"); continue; }
            
            fp = fopen(arg, "rb");
            if (!fp) { perror("Open local file"); continue; }
            
            sdata = pasivo_conn(s_control);
            if (sdata < 0) { fclose(fp); continue; }
            
            if (send_cmd(s_control, reply, sizeof(reply), "STOR %s", arg) < 0) { 
                close(sdata); 
                fclose(fp); 
                continue; 
            }
            
            if (reply[0] != '1') { 
                printf("%s", reply); 
                close(sdata); 
                fclose(fp); 
                continue; 
            }
            
            /* Cerrar fp antes del fork para evitar confusión */
            fclose(fp);
            
            pid = fork();
            if (pid < 0) { 
                perror("fork"); 
                close(sdata); 
                continue; 
            }
            
            if (pid == 0) {
                /* HIJO: enviar archivo */
                FILE *fp_child = fopen(arg, "rb");
                if (!fp_child) { 
                    perror("fopen child"); 
                    close(sdata); 
                    exit(1); 
                }
                
                while ((n = fread(data_buf, 1, sizeof(data_buf), fp_child)) > 0) {
                    size_t sent = 0;
                    while (sent < (size_t)n) {
                        ssize_t w = send(sdata, data_buf + sent, n - sent, 0);
                        if (w < 0) {
                            if (errno == EINTR) continue;
                            perror("send data");
                            fclose(fp_child);
                            close(sdata);
                            exit(1);
                        }
                        sent += w;
                    }
                }
                
                fclose(fp_child);
                close(sdata);
                exit(0);
            } else {
                /* PADRE: leer respuesta final */
                close(sdata);
                printf("Transferencia PUT iniciada (PID %d)\n", pid);
                
                /* Leer respuesta final 226 */
                if (expect_reply(s_control, reply, sizeof(reply)) >= 0) {
                    printf("%s", reply);
                }
                continue;
            }
        }

        if (strcmp(ucmd, "pput") == 0) {
            if (!arg) { printf("Uso: pput <archivo>\n"); continue; }
            
            fp = fopen(arg, "rb");
            if (!fp) { perror("Open local file"); continue; }
            
            int s_listen = -1;
            char port_cmd[128];
            if (configurar_port(&s_listen, port_cmd, sizeof(port_cmd)) < 0) {
                fclose(fp); 
                continue;
            }
            
            if (send_cmd(s_control, reply, sizeof(reply), "%s", port_cmd) < 0) {
                close(s_listen); 
                fclose(fp); 
                continue;
            }
            
            if (send_cmd(s_control, reply, sizeof(reply), "STOR %s", arg) < 0) {
                close(s_listen); 
                fclose(fp); 
                continue;
            }
            
            if (reply[0] != '1') { 
                printf("%s", reply); 
                close(s_listen); 
                fclose(fp); 
                continue; 
            }
            
            fclose(fp);
            
            pid = fork();
            if (pid < 0) { 
                perror("fork"); 
                close(s_listen); 
                continue; 
            }
            
            if (pid == 0) {
                /* HIJO: acepta y envía */
                struct sockaddr_in remote;
                socklen_t alen = sizeof(remote);
                int sdata_child = accept(s_listen, (struct sockaddr*)&remote, &alen);
                close(s_listen);
                
                if (sdata_child < 0) { 
                    perror("accept"); 
                    exit(1); 
                }
                
                FILE *fp_child = fopen(arg, "rb");
                if (!fp_child) { 
                    perror("fopen child"); 
                    close(sdata_child); 
                    exit(1); 
                }
                
                while ((n = fread(data_buf, 1, sizeof(data_buf), fp_child)) > 0) {
                    size_t sent = 0;
                    while (sent < (size_t)n) {
                        ssize_t w = send(sdata_child, data_buf + sent, n - sent, 0);
                        if (w < 0) {
                            if (errno == EINTR) continue;
                            perror("send");
                            break;
                        }
                        sent += w;
                    }
                }
                
                fclose(fp_child);
                close(sdata_child);
                exit(0);
            } else {
                /* PADRE: leer respuesta final */
                close(s_listen);
                printf("Transferencia PPUT iniciada (PID %d)\n", pid);
                
                /* Leer respuesta final 226 */
                if (expect_reply(s_control, reply, sizeof(reply)) >= 0) {
                    printf("%s", reply);
                }
                continue;
            }
        }
       /* CWD, PWD, MKD, DELE (no concurrentes en general) */
        if (strcmp(ucmd, "cd") == 0) {
            if (!arg) { printf("Uso: cd <dir>\n"); continue; }
            if (send_cmd(s_control, reply, sizeof(reply), "CWD %s", arg) >= 0) printf("%s", reply);
            continue;
        }
        if (strcmp(ucmd, "pwd") == 0) {
            if (send_cmd(s_control, reply, sizeof(reply), "PWD") >= 0) printf("%s", reply);
            continue;
        }
        if (strcmp(ucmd, "mkd") == 0) {
            if (!arg) { printf("Uso: mkd <dir>\n"); continue; }
            if (send_cmd(s_control, reply, sizeof(reply), "MKD %s", arg) >= 0) printf("%s", reply);
            continue;
        }
        if (strcmp(ucmd, "dele") == 0) {
            if (!arg) { printf("Uso: dele <file>\n"); continue; }
            if (send_cmd(s_control, reply, sizeof(reply), "DELE %s", arg) >= 0) printf("%s", reply);
            continue;
        }

        /* Comando no reconocido */
        printf("%s: comando no implementado. Escriba 'help' para ver los comandos disponibles.\n", ucmd);
    }

    close(s_control);
    return 0;
}