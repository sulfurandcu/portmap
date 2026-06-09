#include "portmap.h"

extern PortMapping mappings[MAX_CONNECTIONS];
extern Connection connections[MAX_CONNECTIONS];
extern int mapping_count;
extern int connection_count;
extern HANDLE log_mutex;
extern volatile int running;

int main(int argc, char* argv[]) {
    init_paths();
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        printf("WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }
    
    log_mutex = CreateMutex(NULL, FALSE, NULL);
    if (!log_mutex) {
        printf("Failed to create log mutex\n");
        WSACleanup();
        return 1;
    }
    
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
    
    if (argc == 1) {
        print_usage();
        WSACleanup();
        CloseHandle(log_mutex);
        return 1;
    }
    
    PortMapping mapping;
    if (!parse_command_line(argc, argv, &mapping)) {
        WSACleanup();
        CloseHandle(log_mutex);
        return 1;
    }
    
    if (!start_port_mapping(&mapping)) {
        WSACleanup();
        CloseHandle(log_mutex);
        return 1;
    }
    
    mappings[mapping_count++] = mapping;
    
    printf("Port forwarding started. Press Ctrl+C to stop.\n");
    
    while (running) {
        Sleep(1000);
    }
    
    for (int i = 0; i < mapping_count; i++) {
        cleanup_mapping(&mappings[i]);
    }
    
    WSACleanup();
    CloseHandle(log_mutex);
    
    return 0;
}
