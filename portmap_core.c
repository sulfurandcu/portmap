#include "portmap.h"

PortMapping mappings[MAX_CONNECTIONS];
Connection connections[MAX_CONNECTIONS];
int mapping_count = 0;
int connection_count = 0;
HANDLE log_mutex;
volatile int running = 1;

char log_file_path[MAX_PATH + 64];
char config_file_path[MAX_PATH + 64];

void init_paths() {
    const char* user_profile = getenv("USERPROFILE");
    if (user_profile == NULL) {
        // Fallback to current directory if USERPROFILE is not found
        strcpy(log_file_path, "portmap.log");
        strcpy(config_file_path, "portmap_config.ini");
        return;
    }

    char app_dir[MAX_PATH + 32];
    snprintf(app_dir, sizeof(app_dir), "%s\\.portmap", user_profile);

    // Create the directory if it doesn't exist
    CreateDirectory(app_dir, NULL);

    snprintf(log_file_path, sizeof(log_file_path), "%s\\portmap.log", app_dir);
    snprintf(config_file_path, sizeof(config_file_path), "%s\\portmap_config.ini", app_dir);
}

void log_message(const char* format, ...) {
    time_t now;
    struct tm* timeinfo;
    char timestamp[64];
    char message[MAX_LOG_ENTRY];

    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Get current time
    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

    char formatted_log_entry[MAX_LOG_ENTRY + 128]; // More space for timestamp and message
    snprintf(formatted_log_entry, sizeof(formatted_log_entry), "[%s] %s", timestamp, message);

    WaitForSingleObject(log_mutex, INFINITE);

    // Write to log file
    FILE* log_file = fopen(LOG_FILE, "a");
    if (log_file) {
        fprintf(log_file, "%s\n", formatted_log_entry);
        fclose(log_file);
    }

    // Also send to GUI log
#ifdef _GUI_BUILD
    gui_log_message(formatted_log_entry);
#endif

    ReleaseMutex(log_mutex);
}

int parse_address(const char* address, char* host, int* port) {
    char* colon = strchr(address, ':');
    if (!colon) {
        return 0;
    }

    int host_len = colon - address;
    strncpy(host, address, host_len);
    host[host_len] = '\0';

    *port = atoi(colon + 1);
    return (*port > 0 && *port <= 65535);
}

int check_client_allowed(SOCKET client_socket, const char* bind_client) {
    if (strlen(bind_client) == 0) {
        return 1;
    }

    struct sockaddr_storage client_addr;
    int addr_len = sizeof(client_addr);

    if (getpeername(client_socket, (struct sockaddr*)&client_addr, &addr_len) == 0) {
        char client_ip[INET6_ADDRSTRLEN];

        if (client_addr.ss_family == AF_INET) {
            struct sockaddr_in* addr_in = (struct sockaddr_in*)&client_addr;
            inet_ntop(AF_INET, &addr_in->sin_addr, client_ip, INET_ADDRSTRLEN);
        } else {
            struct sockaddr_in6* addr_in6 = (struct sockaddr_in6*)&client_addr;
            inet_ntop(AF_INET6, &addr_in6->sin6_addr, client_ip, INET6_ADDRSTRLEN);
        }

        return strcmp(client_ip, bind_client) == 0;
    }

    return 0;
}

/* Format peer address for logging (getpeername). */
static void format_socket_peer(SOCKET s, char* out, size_t out_len) {
    if (s == INVALID_SOCKET) {
        snprintf(out, out_len, "(无效套接字)");
        return;
    }
    struct sockaddr_storage ss;
    int len = sizeof(ss);
    if (getpeername(s, (struct sockaddr*)&ss, &len) != 0) {
        snprintf(out, out_len, "(无法获取对端: %d)", WSAGetLastError());
        return;
    }
    char ip[INET6_ADDRSTRLEN];
    if (ss.ss_family == AF_INET) {
        struct sockaddr_in* a = (struct sockaddr_in*)&ss;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        snprintf(out, out_len, "%s:%d", ip, (int)ntohs(a->sin_port));
    } else if (ss.ss_family == AF_INET6) {
        struct sockaddr_in6* a6 = (struct sockaddr_in6*)&ss;
        inet_ntop(AF_INET6, &a6->sin6_addr, ip, sizeof(ip));
        snprintf(out, out_len, "[%s]:%d", ip, (int)ntohs(a6->sin6_port));
    } else {
        snprintf(out, out_len, "(未知地址族)");
    }
}

/* 全双工大文件：尽量加大内核缓冲；非阻塞 + select 避免双线程在阻塞 send/recv 上长时间互等 */
#define TCP_RELAY_SOCKBUF (2 * 1024 * 1024)
#define TCP_RELAY_IOWAIT_SEC 120

static void tcp_relay_set_socket_buffers(SOCKET s) {
    int sz = TCP_RELAY_SOCKBUF;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
}

static void tcp_relay_tune_tcp_socket(SOCKET s) {
    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof(nd));
}

static void tcp_relay_set_nonblocking(SOCKET s) {
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
}

/* 非阻塞 send：写满 len；WSAEWOULDBLOCK 时 select 可写再继续（大文件时避免两向同时堵在阻塞 send） */
static int tcp_send_all(SOCKET s, const char* buf, int len) {
    int sent_total = 0;
    while (sent_total < len) {
        int n = send(s, buf + sent_total, len - sent_total, 0);
        if (n > 0) {
            sent_total += n;
            continue;
        }
        if (n == 0) {
            return -1;
        }
        if (n < 0) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) {
                fd_set wf;
                struct timeval tv;
                FD_ZERO(&wf);
                FD_SET(s, &wf);
                tv.tv_sec = TCP_RELAY_IOWAIT_SEC;
                tv.tv_usec = 0;
                if (select(0, NULL, &wf, NULL, &tv) <= 0) {
                    return -1;
                }
                continue;
            }
            return -1;
        }
    }
    return sent_total;
}

/* 非阻塞 recv：返回 >0 / 0 / -1 */
static int tcp_recv_relay(SOCKET s, char* buf, int len) {
    for (;;) {
        int n = recv(s, buf, len, 0);
        if (n > 0) {
            return n;
        }
        if (n == 0) {
            return 0;
        }
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            fd_set rf;
            struct timeval tv;
            FD_ZERO(&rf);
            FD_SET(s, &rf);
            tv.tv_sec = TCP_RELAY_IOWAIT_SEC;
            tv.tv_usec = 0;
            if (select(0, &rf, NULL, NULL, &tv) <= 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
}

static void tcp_relay_notify_peer_done(Connection* conn) {
    LONG v = InterlockedIncrement(&conn->relay_exit_count);
    if (v == 1) {
        if (conn->client_socket != INVALID_SOCKET) {
            shutdown(conn->client_socket, SD_BOTH);
        }
        if (conn->target_socket != INVALID_SOCKET) {
            shutdown(conn->target_socket, SD_BOTH);
        }
    }
}

static void log_tcp_relay_end_client_reason(Connection* conn, int recv_result, int wsa_err) {
    PortMapping* m = conn->mapping;
    char client_peer[128];
    char target_peer[128];
    format_socket_peer(conn->client_socket, client_peer, sizeof(client_peer));
    format_socket_peer(conn->target_socket, target_peer, sizeof(target_peer));
    if (!m) {
        log_message("转发结束: %s -> %s", client_peer, target_peer);
        return;
    }
    if (recv_result == 0) {
        log_message("连接断开: %s -> %s",
                    client_peer, target_peer);
    } else {
        log_message("连接异常: %s -> %s (recv错误 %d)",
                    client_peer, target_peer, wsa_err);
    }
}

/* 双线程全双工：避免单线程里 send 阻塞时无法 recv 另一方向（传几 MB 文件时死锁/假死） */
static DWORD WINAPI tcp_relay_client_to_target(LPVOID param) {
    Connection* conn = (Connection*)param;
    char buffer[BUFFER_SIZE];
    while (conn->active && running) {
        int n = tcp_recv_relay(conn->client_socket, buffer, BUFFER_SIZE);
        if (n < 0) {
            int wsa_err = WSAGetLastError();
            log_tcp_relay_end_client_reason(conn, n, wsa_err);
            break;
        }
        if (n == 0) {
            log_tcp_relay_end_client_reason(conn, 0, 0);
            shutdown(conn->target_socket, SD_SEND);
            break;
        }
        if (tcp_send_all(conn->target_socket, buffer, n) <= 0) {
            break;
        }
    }
    tcp_relay_notify_peer_done(conn);
    return 0;
}

static DWORD WINAPI tcp_relay_target_to_client(LPVOID param) {
    Connection* conn = (Connection*)param;
    char buffer[BUFFER_SIZE];
    while (conn->active && running) {
        int n = tcp_recv_relay(conn->target_socket, buffer, BUFFER_SIZE);
        if (n <= 0) {
            break;
        }
        if (tcp_send_all(conn->client_socket, buffer, n) <= 0) {
            break;
        }
    }
    tcp_relay_notify_peer_done(conn);
    return 0;
}

DWORD WINAPI tcp_forward_thread(LPVOID param) {
    Connection* conn = (Connection*)param;
    HANDLE h_c2t = CreateThread(NULL, 0, tcp_relay_client_to_target, conn, 0, NULL);
    HANDLE h_t2c = CreateThread(NULL, 0, tcp_relay_target_to_client, conn, 0, NULL);
    if (h_c2t) {
        WaitForSingleObject(h_c2t, INFINITE);
        CloseHandle(h_c2t);
    }
    if (h_t2c) {
        WaitForSingleObject(h_t2c, INFINITE);
        CloseHandle(h_t2c);
    }
    cleanup_connection(conn);
    return 0;
}

DWORD WINAPI connection_handler(LPVOID param) {
    PortMapping* mapping = (PortMapping*)param;
    struct sockaddr_storage client_addr;
    int addr_len = sizeof(client_addr);

    while (mapping->active && running) {
        // Check if socket is still valid before accepting
        if (mapping->socket == INVALID_SOCKET) {
            log_message("Socket已关闭，停止连接处理器");
            break;
        }

        SOCKET client_socket = accept(mapping->socket, (struct sockaddr*)&client_addr, &addr_len);

        if (client_socket == INVALID_SOCKET) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                Sleep(100);
                continue;
            } else if (error == WSAENOTSOCK || error == WSAEINVAL) {
                log_message("Socket变为无效，停止处理器 (错误: %d)", error);
                break;
            } else {
                log_message("接受连接失败: %d", error);
                break;
            }
        }

        tcp_relay_set_socket_buffers(client_socket);
        tcp_relay_tune_tcp_socket(client_socket);

        if (!check_client_allowed(client_socket, mapping->bind_client)) {
            char client_ip[INET6_ADDRSTRLEN];
            if (client_addr.ss_family == AF_INET) {
                inet_ntop(AF_INET, &((struct sockaddr_in*)&client_addr)->sin_addr, client_ip, INET_ADDRSTRLEN);
            } else {
                inet_ntop(AF_INET6, &((struct sockaddr_in6*)&client_addr)->sin6_addr, client_ip, INET6_ADDRSTRLEN);
            }
            log_message("来自%s的连接被拒绝（不在绑定列表中）", client_ip);
            closesocket(client_socket);
            continue;
        }

        SOCKET target_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (target_socket == INVALID_SOCKET) {
            log_message("创建目标socket失败: %d", WSAGetLastError());
            closesocket(client_socket);
            continue;
        }

        struct sockaddr_in target_addr;
        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(mapping->remote_port);

        if (inet_pton(AF_INET, mapping->remote_host, &target_addr.sin_addr) != 1) {
            log_message("无效的远程主机: %s", mapping->remote_host);
            closesocket(client_socket);
            closesocket(target_socket);
            continue;
        }

        if (connect(target_socket, (struct sockaddr*)&target_addr, sizeof(target_addr)) != 0) {
            log_message("连接到%s:%d失败", mapping->remote_host, mapping->remote_port);
            closesocket(client_socket);
            closesocket(target_socket);
            continue;
        }

        tcp_relay_set_socket_buffers(target_socket);
        tcp_relay_tune_tcp_socket(target_socket);
        tcp_relay_set_nonblocking(client_socket);
        tcp_relay_set_nonblocking(target_socket);

        if (connection_count < MAX_CONNECTIONS) {
            Connection* conn = &connections[connection_count];
            conn->client_socket = client_socket;
            conn->target_socket = target_socket;
            conn->mapping = mapping;
            conn->active = 1;
            conn->relay_exit_count = 0;

            conn->thread_handle = CreateThread(NULL, 0, tcp_forward_thread, conn, 0, NULL);
            if (conn->thread_handle) {
                connection_count++;
                char client_ip[INET6_ADDRSTRLEN];
                if (client_addr.ss_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in*)&client_addr)->sin_addr, client_ip, INET_ADDRSTRLEN);
                } else {
                    inet_ntop(AF_INET6, &((struct sockaddr_in6*)&client_addr)->sin6_addr, client_ip, INET6_ADDRSTRLEN);
                }
                log_message("连接建立: %s:%d -> %s:%d",
                           client_ip, ntohs(((struct sockaddr_in*)&client_addr)->sin_port),
                           mapping->remote_host, mapping->remote_port);
            } else {
                closesocket(client_socket);
                closesocket(target_socket);
            }
        } else {
            log_message("已达到最大连接数");
            closesocket(client_socket);
            closesocket(target_socket);
        }
    }

    return 0;
}

DWORD WINAPI udp_forward_thread(LPVOID param) {
    PortMapping* mapping = (PortMapping*)param;
    char buffer[BUFFER_SIZE];
    struct sockaddr_storage client_addr;
    int addr_len = sizeof(client_addr);

    while (mapping->active && running) {
        // Check if socket is still valid before receiving
        if (mapping->udp_socket == INVALID_SOCKET) {
            log_message("UDP socket已关闭，停止转发线程");
            break;
        }

        int bytes_received = recvfrom(mapping->udp_socket, buffer, BUFFER_SIZE, 0,
                                     (struct sockaddr*)&client_addr, &addr_len);

        if (bytes_received > 0) {
            if (strlen(mapping->bind_client) > 0) {
                char client_ip[INET6_ADDRSTRLEN] = {0};
                if (client_addr.ss_family == AF_INET) {
                    inet_ntop(AF_INET, &((struct sockaddr_in*)&client_addr)->sin_addr, client_ip, INET_ADDRSTRLEN);
                } else if (client_addr.ss_family == AF_INET6) {
                    inet_ntop(AF_INET6, &((struct sockaddr_in6*)&client_addr)->sin6_addr, client_ip, INET6_ADDRSTRLEN);
                }
                if (strcmp(client_ip, mapping->bind_client) != 0) {
                    continue;
                }
            }

            SOCKET target_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (target_socket != INVALID_SOCKET) {
                struct sockaddr_in target_addr;
                memset(&target_addr, 0, sizeof(target_addr));
                target_addr.sin_family = AF_INET;
                target_addr.sin_port = htons(mapping->remote_port);

                if (inet_pton(AF_INET, mapping->remote_host, &target_addr.sin_addr) == 1) {
                    sendto(target_socket, buffer, bytes_received, 0,
                          (struct sockaddr*)&target_addr, sizeof(target_addr));
                }
                closesocket(target_socket);
            }
        }
    }

    return 0;
}

int create_tcp_listener(PortMapping* mapping) {
    mapping->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (mapping->socket == INVALID_SOCKET) {
        log_message("创建TCP socket失败: %d", WSAGetLastError());
        return 0;
    }

    u_long mode = 1;
    ioctlsocket(mapping->socket, FIONBIO, &mode);

    int reuse_addr = 1;
    setsockopt(mapping->socket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse_addr, sizeof(reuse_addr));

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(mapping->local_port);

    if (bind(mapping->socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) != 0) {
        log_message("绑定TCP端口%d失败: %d", mapping->local_port, WSAGetLastError());
        closesocket(mapping->socket);
        return 0;
    }

    if (listen(mapping->socket, SOMAXCONN) != 0) {
        log_message("监听TCP端口%d失败: %d", mapping->local_port, WSAGetLastError());
        closesocket(mapping->socket);
        return 0;
    }

    return 1;
}

int create_udp_listener(PortMapping* mapping) {
    mapping->udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mapping->udp_socket == INVALID_SOCKET) {
        log_message("创建UDP socket失败: %d", WSAGetLastError());
        return 0;
    }

    int reuse_addr = 1;
    setsockopt(mapping->udp_socket, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse_addr, sizeof(reuse_addr));

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(mapping->local_port);

    if (bind(mapping->udp_socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) != 0) {
        log_message("绑定UDP端口%d失败: %d", mapping->local_port, WSAGetLastError());
        closesocket(mapping->udp_socket);
        mapping->udp_socket = INVALID_SOCKET;
        return 0;
    }

    return 1;
}

int start_port_mapping(PortMapping* mapping) {
    // Prevent duplicate startup
    if (mapping->active || mapping->thread_handle != NULL || mapping->udp_thread_handle != NULL) {
        return 1; // Already active or thread exists
    }

    int tcp_ok = create_tcp_listener(mapping);
    int udp_ok = create_udp_listener(mapping);
    int success = tcp_ok && udp_ok;
    if (!success) {
        if (mapping->socket != INVALID_SOCKET) {
            closesocket(mapping->socket);
            mapping->socket = INVALID_SOCKET;
        }
        if (mapping->udp_socket != INVALID_SOCKET) {
            closesocket(mapping->udp_socket);
            mapping->udp_socket = INVALID_SOCKET;
        }
    }

    if (success) {
        mapping->active = 1;

        DWORD thread_id;
        mapping->thread_handle = CreateThread(NULL, 0, connection_handler, mapping, 0, &thread_id);
        mapping->udp_thread_handle = CreateThread(NULL, 0, udp_forward_thread, mapping, 0, &thread_id);

        if (mapping->thread_handle && mapping->udp_thread_handle) {
            if (strlen(mapping->bind_client) > 0) {
                log_message("启动转发: %d -> %s:%d 指定主机: %s",
                        mapping->local_port, mapping->remote_host, mapping->remote_port, mapping->bind_client);
            }
            else {
                log_message("启动转发: %d -> %s:%d",
                        mapping->local_port, mapping->remote_host, mapping->remote_port);
            }
            return 1;
        } else {
            log_message("创建转发线程失败");
            if (mapping->thread_handle) {
                CloseHandle(mapping->thread_handle);
                mapping->thread_handle = NULL;
            }
            if (mapping->udp_thread_handle) {
                CloseHandle(mapping->udp_thread_handle);
                mapping->udp_thread_handle = NULL;
            }
            if (mapping->socket != INVALID_SOCKET) {
                closesocket(mapping->socket);
                mapping->socket = INVALID_SOCKET;
            }
            if (mapping->udp_socket != INVALID_SOCKET) {
                closesocket(mapping->udp_socket);
                mapping->udp_socket = INVALID_SOCKET;
            }
            mapping->active = 0;
            return 0;
        }
    }

    return 0;
}

/* Before closing listeners, gracefully shut down established TCP relays for this mapping:
 * shutdown(SD_SEND) sends FIN so the client and upstream see the connection closing. */
static void shutdown_tcp_relays_for_mapping(PortMapping* mapping) {
    int n = 0;
    for (int i = 0; i < connection_count; i++) {
        Connection* conn = &connections[i];
        if (!conn->active || conn->mapping != mapping) {
            continue;
        }
        n++;
    }
    if (n > 0) {
        log_message("通知关闭 %d 条已建立的TCP连接", n);
    }
    for (int i = 0; i < connection_count; i++) {
        Connection* conn = &connections[i];
        if (!conn->active || conn->mapping != mapping) {
            continue;
        }
        char client_peer[128];
        char target_peer[128];
        format_socket_peer(conn->client_socket, client_peer, sizeof(client_peer));
        format_socket_peer(conn->target_socket, target_peer, sizeof(target_peer));
        log_message("关闭连接: %s -> %s", client_peer, target_peer);

        if (conn->client_socket != INVALID_SOCKET) {
            shutdown(conn->client_socket, SD_SEND);
        }
        if (conn->target_socket != INVALID_SOCKET) {
            shutdown(conn->target_socket, SD_SEND);
        }
        conn->active = 0;
    }
    Sleep(50);
}

void stop_port_mapping(PortMapping* mapping) {
    // Only log if the mapping was actually active
    int was_active = mapping->active;

    // First set active flag to false to stop the thread
    mapping->active = 0;

    shutdown_tcp_relays_for_mapping(mapping);

    /* Close listener sockets BEFORE WaitForSingleObject. Otherwise accept()/recvfrom()
     * stay blocked and each worker thread waits out the full timeout (e.g. 3s + 3s). */
    if (mapping->socket != INVALID_SOCKET) {
        closesocket(mapping->socket);
        mapping->socket = INVALID_SOCKET;
    }
    if (mapping->udp_socket != INVALID_SOCKET) {
        closesocket(mapping->udp_socket);
        mapping->udp_socket = INVALID_SOCKET;
    }

    if (mapping->thread_handle) {
        WaitForSingleObject(mapping->thread_handle, 5000);
        CloseHandle(mapping->thread_handle);
        mapping->thread_handle = NULL;
    }
    if (mapping->udp_thread_handle) {
        WaitForSingleObject(mapping->udp_thread_handle, 5000);
        CloseHandle(mapping->udp_thread_handle);
        mapping->udp_thread_handle = NULL;
    }

    // Only log if it was actually active before
    if (was_active) {
        log_message("停止转发: %d -> %s:%d",
                   mapping->local_port, mapping->remote_host, mapping->remote_port);
    }
}

void cleanup_connection(Connection* conn) {
    conn->active = 0;

    if (conn->client_socket != INVALID_SOCKET) {
        closesocket(conn->client_socket);
    }

    if (conn->target_socket != INVALID_SOCKET) {
        closesocket(conn->target_socket);
    }

    if (conn->thread_handle) {
        CloseHandle(conn->thread_handle);
    }
}

void cleanup_mapping(PortMapping* mapping) {
    stop_port_mapping(mapping);
}

void print_usage() {
    log_message("用法: portmap.exe [选项]");
    log_message("选项:");
    log_message("  -l <端口>        要监听的本地端口");
    log_message("  -r <主机:端口>   要转发到的远程主机和端口");
    log_message("  -b <IP>          绑定到特定客户端IP（可选）");
    log_message("  -h               显示此帮助");
    log_message("");
    log_message("示例:");
    log_message("  portmap.exe -l 8080 -r 192.168.1.100:80");
    log_message("  portmap.exe -l 8080 -r 192.168.1.100:80 -b 192.168.1.200");
    log_message("  所有转发同时启用 TCP 与 UDP");
}

int parse_command_line(int argc, char* argv[], PortMapping* mapping) {
    memset(mapping, 0, sizeof(PortMapping));
    mapping->address_family = AF_IPV4;
    mapping->socket = INVALID_SOCKET;
    mapping->udp_socket = INVALID_SOCKET;
    mapping->thread_handle = NULL;
    mapping->udp_thread_handle = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            mapping->local_port = atoi(argv[++i]);
            if (mapping->local_port <= 0 || mapping->local_port > 65535) {
                log_message("无效的本地端口: %s", argv[i]);
                return 0;
            }
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            if (!parse_address(argv[++i], mapping->remote_host, &mapping->remote_port)) {
                log_message("无效的远程地址: %s", argv[i]);
                return 0;
            }
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            strcpy(mapping->bind_client, argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            i++; /* 兼容旧脚本，-p 已无含义 */
        } else if (strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        } else {
            log_message("未知选项: %s", argv[i]);
            return 0;
        }
    }

    if (mapping->local_port == 0 || strlen(mapping->remote_host) == 0 || mapping->remote_port == 0) {
        log_message("缺少必需参数");
        print_usage();
        return 0;
    }

    return 1;
}

int count_active_connections_for_mapping(const PortMapping* mapping) {
    int n = 0;
    for (int i = 0; i < connection_count; i++) {
        if (connections[i].active && connections[i].mapping == mapping) {
            n++;
        }
    }
    return n;
}

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT) {
        running = 0;
        log_message("收到关闭信号，停止所有映射...");

        for (int i = 0; i < mapping_count; i++) {
            stop_port_mapping(&mappings[i]);
        }

        for (int i = 0; i < connection_count; i++) {
            if (connections[i].active) {
                cleanup_connection(&connections[i]);
            }
        }

        return TRUE;
    }
    return FALSE;
}

int save_config(const char* filename, PortMapping* mappings, int count) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        return 0;
    }

    fprintf(file, "[PortMapConfig]\n");
    fprintf(file, "count=%d\n", count);
    fprintf(file, "\n");

    for (int i = 0; i < count; i++) {
        fprintf(file, "[Mapping%d]\n", i);
        fprintf(file, "name=%s\n", mappings[i].name);
        fprintf(file, "local_port=%d\n", mappings[i].local_port);
        fprintf(file, "remote_host=%s\n", mappings[i].remote_host);
        fprintf(file, "remote_port=%d\n", mappings[i].remote_port);
        fprintf(file, "bind_client=%s\n", mappings[i].bind_client);
        fprintf(file, "address_family=%s\n", mappings[i].address_family == AF_IPV4 ? "ipv4" : "ipv6");
        fprintf(file, "active=%d\n", mappings[i].active);
        fprintf(file, "\n");
    }

    fclose(file);
    return 1;
}

int load_config(const char* filename, PortMapping* mappings, int* count) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        return 0;
    }

    char line[512];
    int current_mapping = -1;
    *count = 0;

    while (fgets(line, sizeof(line), file)) {
        // Remove newline
        line[strcspn(line, "\r\n")] = 0;

        if (strlen(line) == 0 || line[0] == '#' || line[0] == ';') {
            continue;
        }

        if (line[0] == '[') {
            if (strstr(line, "[Mapping")) {
                current_mapping = atoi(line + 8);
                if (current_mapping >= 0 && current_mapping < MAX_MAPPINGS) {
                    memset(&mappings[current_mapping], 0, sizeof(PortMapping));
                    mappings[current_mapping].socket = INVALID_SOCKET;
                    mappings[current_mapping].udp_socket = INVALID_SOCKET;
                }
            }
        } else if (current_mapping >= 0 && current_mapping < MAX_MAPPINGS) {
            char* key = strtok(line, "=");
            char* value = strtok(NULL, "=");

            if (key && value) {
                if (strcmp(key, "name") == 0) {
                    strncpy(mappings[current_mapping].name, value, sizeof(mappings[current_mapping].name) - 1);
                    mappings[current_mapping].name[sizeof(mappings[current_mapping].name) - 1] = '\0';
                } else if (strcmp(key, "local_port") == 0) {
                    mappings[current_mapping].local_port = atoi(value);
                } else if (strcmp(key, "remote_host") == 0) {
                    strcpy(mappings[current_mapping].remote_host, value);
                } else if (strcmp(key, "remote_port") == 0) {
                    mappings[current_mapping].remote_port = atoi(value);
                } else if (strcmp(key, "bind_client") == 0) {
                    strcpy(mappings[current_mapping].bind_client, value);
                } else if (strcmp(key, "address_family") == 0) {
                    mappings[current_mapping].address_family = strcmp(value, "ipv6") == 0 ? AF_IPV6 : AF_IPV4;
                } else if (strcmp(key, "active") == 0) {
                    mappings[current_mapping].active = atoi(value);
                }
            }
        }
    }

    fclose(file);

    // Count actual mappings
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        if (mappings[i].local_port > 0 && strlen(mappings[i].remote_host) > 0) {
            (*count)++;
        }
    }

    return 1;
}
