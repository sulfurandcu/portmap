#ifndef PORTMAP_H
#define PORTMAP_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif

#define MAX_CONNECTIONS 256
#define MAX_MAPPINGS 256
#define BUFFER_SIZE (64 * 1024)
extern char log_file_path[MAX_PATH + 64];
extern char config_file_path[MAX_PATH + 64];
#define LOG_FILE log_file_path
#define CONFIG_FILE config_file_path
void init_paths();
#define MAX_LOG_ENTRY 512

typedef enum {
    AF_IPV4,
    AF_IPV6
} AddressFamily;

typedef struct {
    char name[64];
    int local_port;
    char remote_host[256];
    int remote_port;
    char bind_client[256];
    AddressFamily address_family;
    SOCKET socket;
    HANDLE thread_handle;
    SOCKET udp_socket;
    HANDLE udp_thread_handle;
    int active;
} PortMapping;

typedef struct {
    SOCKET client_socket;
    SOCKET target_socket;
    PortMapping* mapping;
    HANDLE thread_handle;
    int active;
    volatile LONG relay_exit_count; /* 双线程转发：首个退出方负责 shutdown 唤醒另一方向 */
} Connection;

extern volatile int running;
extern HANDLE log_mutex;

// Function to send log messages to GUI
#ifdef _GUI_BUILD
void gui_log_message(const char* message);
#else
// Dummy function for CLI build
static inline void gui_log_message(const char* message) { (void)message; }
#endif

void log_message(const char* format, ...);
int parse_address(const char* address, char* host, int* port);
int create_tcp_listener(PortMapping* mapping);
int create_udp_listener(PortMapping* mapping);
DWORD WINAPI tcp_forward_thread(LPVOID param);
DWORD WINAPI udp_forward_thread(LPVOID param);
DWORD WINAPI connection_handler(LPVOID param);
int check_client_allowed(SOCKET client_socket, const char* bind_client);
void cleanup_mapping(PortMapping* mapping);
void cleanup_connection(Connection* conn);
int start_port_mapping(PortMapping* mapping);
void stop_port_mapping(PortMapping* mapping);
void print_usage();
int parse_command_line(int argc, char* argv[], PortMapping* mapping);
BOOL WINAPI console_ctrl_handler(DWORD ctrl_type);
int save_config(const char* filename, PortMapping* mappings, int count);
int load_config(const char* filename, PortMapping* mappings, int* count);
int count_active_connections_for_mapping(const PortMapping* mapping);

#endif
