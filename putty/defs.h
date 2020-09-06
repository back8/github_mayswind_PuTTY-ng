/*
 * defs.h: initial definitions for PuTTY.
 *
 * The rule about this header file is that it can't depend on any
 * other header file in this code base. This is where we define
 * things, as much as we can, that other headers will want to refer
 * to, such as opaque structure types and their associated typedefs,
 * or macros that are used by other headers.
 */

#ifndef PUTTY_DEFS_H
#define PUTTY_DEFS_H

#include <stddef.h>

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE 1
#endif

typedef struct conf_tag Conf;
typedef struct terminal_tag Terminal;

typedef struct Filename Filename;
typedef struct FontSpec FontSpec;

typedef struct bufchain_tag bufchain;

typedef struct strbuf strbuf;

struct RSAKey;

#include <stdint.h>
typedef uint32_t uint32;

typedef struct BinarySink BinarySink;
typedef struct BinarySource BinarySource;

typedef struct IdempotentCallback IdempotentCallback;

typedef struct SockAddr SockAddr;

typedef struct Socket_vtable Socket_vtable;
typedef struct Plug_vtable Plug_vtable;

typedef struct Backend Backend;
typedef struct Backend_vtable Backend_vtable;

typedef struct Ldisc_tag Ldisc;
typedef struct LogContext_tag LogContext;

typedef struct Frontend Frontend;

typedef struct Ssh Ssh;

typedef struct Channel Channel;
typedef struct SshChannel SshChannel;

typedef struct ssh_sharing_state ssh_sharing_state;
typedef struct ssh_sharing_connstate ssh_sharing_connstate;
typedef struct share_channel share_channel;

typedef struct PortFwdManager PortFwdManager;
typedef struct PortFwdRecord PortFwdRecord;
typedef struct ConnectionLayer ConnectionLayer;

typedef struct dlgparam dlgparam;

typedef struct SessionSpecial SessionSpecial;

typedef const Socket_vtable *Socket;
typedef const Plug_vtable *Plug;

/*
 * A small structure wrapping up a (pointer, length) pair so that it
 * can be conveniently passed to or from a function.
 */
typedef struct ptrlen {
    const void *ptr;
    size_t len;
} ptrlen;

typedef struct logblank_t logblank_t;

typedef struct BinaryPacketProtocol BinaryPacketProtocol;
typedef struct PacketProtocolLayer PacketProtocolLayer;

/* Do a compile-time type-check of 'to_check' (without evaluating it),
 * as a side effect of returning the value 'to_return'. Note that
 * although this macro double-*expands* to_return, it always
 * *evaluates* exactly one copy of it, so it's side-effect safe. */
#define TYPECHECK(to_check, to_return)                  \
    (sizeof(to_check) ? (to_return) : (to_return))

/* Return a pointer to the object of structure type 'type' whose field
 * with name 'field' is pointed at by 'object'. */
#define FROMFIELD(object, type, field)                                  \
    TYPECHECK(object == &((type *)0)->field,                            \
              ((type *)(((char *)(object)) - offsetof(type, field))))

#endif /* PUTTY_DEFS_H */
