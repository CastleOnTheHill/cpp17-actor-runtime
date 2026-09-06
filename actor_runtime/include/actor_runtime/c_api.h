#ifndef ACTOR_RUNTIME_C_API_H
#define ACTOR_RUNTIME_C_API_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct ar_system ar_system;
typedef struct ar_client ar_client;
enum ar_status { AR_OK = 0, AR_INVALID = 1, AR_TIMEOUT = 2, AR_CLOSED = 3, AR_BUFFER_SMALL = 4, AR_ERROR = 5 };
typedef struct ar_message {
    uint64_t type, id, correlation;
    const void* data;
    size_t size;
    /* 仅在 handler 回调期间有效，不得保存此指针；ar_reply 在回调内使用。 */
    const void* internal_request;
} ar_message;
typedef void (*ar_handler)(void* user, ar_system* system, const ar_message* message);
typedef void (*ar_cleanup)(void* user);

const char* ar_last_error(void);
int ar_system_create(const char* process_name, size_t workers, ar_system** result);
/* C++ 宿主可传入 ActorSystem*；包装不接管其所有权，宿主必须最后销毁系统。 */
int ar_system_borrow(void* cpp_actor_system, ar_system** result);
int ar_system_destroy(ar_system* system);
int ar_actor_register(ar_system*, const char* name, ar_handler, void* user, ar_cleanup, uint64_t* actor_id);
int ar_actor_stop(ar_system*, uint64_t actor_id, uint32_t timeout_ms);
int ar_reply(ar_system*, const ar_message*, uint64_t type, const void* data, size_t size);
int ar_client_create(ar_system*, const char* name, ar_client** result);
int ar_client_destroy(ar_client*);
/* 外部线程的同步接口，禁止在 Actor handler / 协程内调用。单个 client 串行请求。
 * 数据在返回前复制到 out；超时后的应答被丢弃，不污染下一次请求。
 * out_size 在成功/缓冲区过小时返回所需大小。C struct 必须无指针并使用相同 ABI。
 */
int ar_client_request(ar_client*, const char* process, const char* actor,
                      uint64_t type, const void* data, size_t size, uint64_t reply_type,
                      uint32_t timeout_ms, void* out, size_t capacity, size_t* out_size);
#ifdef __cplusplus
}
#endif
#endif
