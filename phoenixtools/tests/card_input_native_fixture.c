#define _GNU_SOURCE
#include "card_input_fixture.h"

#define FRAME_SIZE 0xd0u
#define NATIVE_ADVANCE 0x0807e64fu
#define NATIVE_READER 0x0807e67cu
#define NATIVE_CONVERT 0x0807e6f8u
#define NATIVE_INPUT_BASE 0x097be000u
#define NATIVE_BINDING_COUNT 0x097be388u
#define NATIVE_BINDING_TABLE 0x097be38cu
#define STACK_SIZE 65536u

enum native_route { ROUTE_ADVANCE = 1, ROUTE_CONVERT, ROUTE_REAL_READER };
static unsigned char *consumer_stack;
const unsigned char supported_boundary[11] = {
    0x8b, 0x44, 0x24, 0x10, 0x8b, 0x14, 0x85, 0x48, 0x01, 0x7c, 0x09
};

/*
 * Execute the actual installed jump and naked trampoline. ESP points at the
 * native frame without an artificial C return address; slots beyond it hold
 * our saved stack and the consumer route. Callees have 64K of stack below it.
 */
extern unsigned int fixture_run_boundary(unsigned char *frame);
extern void fixture_native_return(void);
__asm__(
    ".text\n"
    ".globl fixture_run_boundary\n"
    ".type fixture_run_boundary,@function\n"
    "fixture_run_boundary:\n"
    "pushl %ebp\n"
    "pushl %ebx\n"
    "pushl %esi\n"
    "pushl %edi\n"
    "movl 20(%esp), %eax\n"
    "movl %esp, 0xe0(%eax)\n"
    "movl %eax, %esp\n"
    "movl $0x0807e671, %eax\n"
    "jmp *%eax\n"
    ".size fixture_run_boundary,.-fixture_run_boundary\n"
    ".globl fixture_native_return\n"
    ".hidden fixture_native_return\n"
    ".type fixture_native_return,@function\n"
    "fixture_native_return:\n"
    "movl 0xd0(%esp), %eax\n"
    "movl 0xe0(%esp), %esp\n"
    "popl %edi\n"
    "popl %esi\n"
    "popl %ebx\n"
    "popl %ebp\n"
    "ret\n"
    ".size fixture_native_return,.-fixture_native_return\n");

static void map_at(uintptr_t address, size_t length)
{
    void *mapped = mmap((void *)address, length,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mapped == (void *)address);
}

uint32_t *native_count(void)
{
    return (uint32_t *)(uintptr_t)NATIVE_BINDING_COUNT;
}

struct native_binding *native_bindings(void)
{
    return (struct native_binding *)(uintptr_t)NATIVE_BINDING_TABLE;
}

void map_native_inputs(void)
{
    unsigned int index;

    map_at(NATIVE_INPUT_BASE, 0x4000u);
    *native_count() = 5;
    for (index = 0; index < *native_count(); ++index) {
        native_bindings()[index].reserved = 0x1357u;
        native_bindings()[index].action_id = 0x100u + index;
    }
}

static void install_native_consumer(uintptr_t address, enum native_route route)
{
    /* movl $route,0xd0(%esp); movl $return,%eax; jmp *%eax */
    unsigned char code[] = {
        0xc7, 0x84, 0x24, 0xd0, 0, 0, 0, 0, 0, 0, 0,
        0xb8, 0, 0, 0, 0, 0xff, 0xe0
    };
    uint32_t result = (uint32_t)route;
    uint32_t continuation = (uint32_t)(uintptr_t)fixture_native_return;

    memcpy(code + 7, &result, sizeof(result));
    memcpy(code + 12, &continuation, sizeof(continuation));
    memcpy((void *)address, code, sizeof(code));
}

void map_native_card(void)
{
    map_at(CODE_BASE, 4096u);
    install_native_consumer(NATIVE_ADVANCE, ROUTE_ADVANCE);
    install_native_consumer(NATIVE_READER, ROUTE_REAL_READER);
    install_native_consumer(NATIVE_CONVERT, ROUTE_CONVERT);
    memcpy((void *)(uintptr_t)CARD_BOUNDARY, supported_boundary,
           sizeof(supported_boundary));
    consumer_stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(consumer_stack != MAP_FAILED);
}

int native_has_key(unsigned int code)
{
    uint32_t index;

    for (index = 0; index < *native_count(); ++index)
        if (native_bindings()[index].mode == 0 &&
            native_bindings()[index].key_index == code - 8u)
            return 1;
    return 0;
}

void check_native_install(int lxio)
{
    enum pt_input_action action;

    pt_input_install_phoenix_bindings();
    assert(*native_count() == (lxio ? 5u : 5u + PT_INPUT_CARD1));
    for (action = PT_INPUT_UPPER_LEFT; action < PT_INPUT_CARD1; ++action)
        assert(native_has_key(pt_binding_get(action)->keycode) == !lxio);
    assert(!native_has_key(keycode("F3")));
    assert(!native_has_key(keycode("F4")));
}

void expect_frame(unsigned int player, const unsigned char *uid,
                  size_t length, int trampoline)
{
    unsigned char local[FRAME_SIZE];
    unsigned char expected[FRAME_SIZE];
    unsigned char *frame = trampoline ? consumer_stack + STACK_SIZE - 0x100u : local;
    uint32_t native_player = player;
    uint32_t native_length = (uint32_t)length;

    memset(frame, 0xa5, FRAME_SIZE);
    memcpy(frame + 0x10, &native_player, sizeof(native_player));
    memcpy(expected, frame, sizeof(expected));
    if (length) {
        memset(expected + 0x4c, 0, 0x84);
        memcpy(expected + 0x4c, uid, length);
        memcpy(expected + 0xcc, &native_length, sizeof(native_length));
    }
    if (trampoline)
        assert(fixture_run_boundary(frame) ==
               (unsigned int)(length ? ROUTE_CONVERT : ROUTE_ADVANCE));
    else
        assert(pt_network_card_event(frame) == (length != 0));
    assert(!memcmp(frame, expected, sizeof(expected)));
}

void test_patch_mismatch(unsigned int byte)
{
    unsigned char before[4096];
    unsigned char *boundary = (unsigned char *)(uintptr_t)CARD_BOUNDARY;

    assert(byte < sizeof(supported_boundary));
    setup(1, 0, "");
    map_native_card();
    boundary[byte] ^= 1u;
    memcpy(before, (void *)(uintptr_t)CODE_BASE, sizeof(before));
    assert(pt_network_card_initialize() != 0);
    assert(open_calls == 0);
    assert(!memcmp(before, (void *)(uintptr_t)CODE_BASE, sizeof(before)));
}

void test_unmapped_patch(unsigned int unused)
{
    (void)unused;
    setup(1, 0, "");
    assert(pt_network_card_initialize() != 0);
    assert(open_calls == 0);
}
