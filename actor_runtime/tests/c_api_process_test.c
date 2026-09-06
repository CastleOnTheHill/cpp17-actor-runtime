/* 本文件由 C 编译器编译，验证无 C++ 模板、异常或 std::any 泄漏到 C 模块。 */
#include "actor_runtime/c_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>

struct business_packet { uint64_t sequence; int32_t value; char label[20]; };
static atomic_int stopping = 0;
static void require(int ok, const char* text) {
    if (!ok) { fprintf(stderr, "[FAIL] %s: %s\n", text, ar_last_error()); exit(1); }
}
static void echo(void* user, ar_system* system, const ar_message* message) {
    int increment = *(int*)user;
    if (message->type == 9) {
        require(ar_reply(system, message, 10, NULL, 0) == AR_OK, "stop reply");
        atomic_store(&stopping, 1);
        return;
    }
    if (message->type == 4) { require(ar_reply(system, message, 5, NULL, 0) == AR_OK, "empty reply"); return; }
    require(message->size == sizeof(struct business_packet), "packet size");
    struct business_packet result;
    memcpy(&result, message->data, sizeof(result));
    result.value += increment;
    require(ar_reply(system, message, 2, &result, sizeof(result)) == AR_OK, "business reply");
}
int main(int argc, char** argv) {
    require(argc >= 3, "arguments");
    if (strcmp(argv[1], "server") == 0) {
        ar_system* system = NULL;
        uint64_t id = 0;
        int increment = atoi(argv[3]);
        require(ar_system_create(argv[2], 2, &system) == AR_OK, "create server");
        require(ar_actor_register(system, "echo", echo, &increment, NULL, &id) == AR_OK, "register server actor");
        puts("READY"); fflush(stdout);
        while (!atomic_load(&stopping)) usleep(1000);
        require(ar_system_destroy(system) == AR_OK, "destroy server");
        puts("[PASS] C server shutdown");
        return 0;
    }
    char local_name[64];
    snprintf(local_name, sizeof(local_name), "c_client_%ld", (long)getpid());
    ar_system* system = NULL;
    ar_client* client = NULL;
    require(ar_system_create(local_name, 2, &system) == AR_OK, "create client system");
    /* 两个服务进程与本地 C client 都使用 echo，验证进程名维度隔离。 */
    require(ar_client_create(system, "echo", &client) == AR_OK, "create bridge");
    for (int server = 0; server < 2; ++server) {
        const char* process = argv[2 + server];
        for (int i = 0; i < 50; ++i) {
            struct business_packet input = {0}, output = {0};
            input.sequence = (uint64_t)i; input.value = 40; strcpy(input.label, "memcpy packet");
            size_t written = 0;
            require(ar_client_request(client, process, "echo", 1, &input, sizeof(input), 2, 2000,
                                      &output, sizeof(output), &written) == AR_OK, "C request/reply");
            require(written == sizeof(output) && output.value == 41 + server && output.sequence == (uint64_t)i &&
                    strcmp(output.label, input.label) == 0, "POD bytes or routing corrupted");
        }
    }
    struct business_packet packet = {0};
    size_t written = 0;
    require(ar_client_request(client, argv[2], "echo", 1, &packet, sizeof(packet), 999, 10,
                              &packet, sizeof(packet), &written) == AR_TIMEOUT, "wrong response type must time out");
    require(ar_client_request(client, argv[2], "echo", 1, &packet, sizeof(packet), 2, 2000,
                              &packet, 1, &written) == AR_BUFFER_SMALL && written == sizeof(packet), "small reply buffer");
    require(ar_client_request(client, argv[2], "missing", 1, &packet, sizeof(packet), 2, 10,
                              &packet, sizeof(packet), &written) == AR_TIMEOUT, "missing actor timeout");
    require(ar_client_request(client, "missing_process", "echo", 1, &packet, sizeof(packet), 2, 10,
                              &packet, sizeof(packet), &written) == AR_ERROR, "missing process error");
    require(ar_client_request(client, argv[2], "echo", 1, NULL, 10, 2, 10,
                              &packet, sizeof(packet), &written) == AR_INVALID, "null input");
    require(ar_client_request(client, argv[2], "echo", 4, NULL, 0, 5, 2000,
                              NULL, 0, &written) == AR_OK && written == 0, "empty bytes");
    for (int server = 0; server < 2; ++server)
        require(ar_client_request(client, argv[2 + server], "echo", 9, NULL, 0, 10, 2000,
                                  NULL, 0, &written) == AR_OK, "stop peer");
    require(ar_client_destroy(client) == AR_OK, "destroy client");
    require(ar_system_destroy(system) == AR_OK, "destroy client system");
    puts("[PASS] 100 C/POD cross-process requests, duplicate names, errors, timeouts, shutdown");
    return 0;
}
