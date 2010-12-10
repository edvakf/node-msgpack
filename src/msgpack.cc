/* Bindings for http://msgpack.sourceforge.net/ */

#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <msgpack.h>
#include <math.h>
#include <list>
#include <assert.h>

using namespace v8;
using namespace node;

static Persistent<FunctionTemplate> msgpack_unpack_template;

// An exception class that wraps a textual message
class MsgpackException {
    public:
        MsgpackException(const char *str) :
            msg(String::New(str)) {
        }

        Handle<Value> getThrownException() {
            return Exception::TypeError(msg);
        }

    private:
        const Handle<String> msg;
};

// A holder for a msgpack_zone object; ensures destruction on scope exit
class MsgpackZone {
    public:
        msgpack_zone _mz;

        MsgpackZone(size_t sz = 1024) {
            msgpack_zone_init(&this->_mz, sz);
        }

        ~MsgpackZone() {
            msgpack_zone_destroy(&this->_mz);
        }
};

// A holder for a msgpack_sbuffer object; ensures destruction on scope exit
class MsgpackSbuffer {
    public:
        msgpack_sbuffer _sbuf;

        MsgpackSbuffer() {
            msgpack_sbuffer_init(&this->_sbuf);
        }

        ~MsgpackSbuffer() {
            msgpack_sbuffer_destroy(&this->_sbuf);
        }
};

// Object to check for cycles when packing.
class MsgpackCycle {
    public:
        MsgpackCycle() {
        }

        ~MsgpackCycle() {
            assert(_objs.empty());
            _objs.clear();
        }

        void enter(Handle<Value> v) {
            Handle<Value> o = v;

            for (std::list< Handle<Value> >::iterator iter = _objs.begin();
                 iter != _objs.end();
                 iter++) {
                if ((*iter)->StrictEquals(o)) {
                    this->out();
                    // This message should not change without updating
                    // test/test.js to expect the new text
                    throw MsgpackException( \
                        "Cowardly refusing to pack object with circular reference" \
                    ); 
                }
            }

            _objs.push_back(o);
        }

        void out() {
            _objs.pop_back();
        }

    private:
        std::list< Handle<Value> > _objs;
};

#define DBG_PRINT_BUF(buf, name) \
    do { \
        fprintf(stderr, "Buffer %s has %lu bytes:\n", \
            (name), (buf)->length() \
        ); \
        for (uint32_t i = 0; i * 16 < (buf)->length(); i++) { \
            fprintf(stderr, "  "); \
            for (uint32_t ii = 0; \
                 ii < 16 && (i * 16) + ii < (buf)->length(); \
                 ii++) { \
                fprintf(stderr, "%s%2.2hhx", \
                    (ii > 0 && (ii % 2 == 0)) ? " " : "", \
                    (buf)->data()[i * 16 + ii] \
                ); \
            } \
            fprintf(stderr, "\n"); \
        } \
    } while (0)

// Convert a V8 object to a MessagePack object.
//
// This method is recursive. It will probably blow out the stack on objects
// with extremely deep nesting.
//
// If a circular reference is detected, an exception is thrown.
static void
v8_to_msgpack(Handle<Value> v8obj, msgpack_object *mo, msgpack_zone *mz,
              MsgpackCycle *mc) {

    if (v8obj->IsUndefined() || v8obj->IsNull()) {
        mo->type = MSGPACK_OBJECT_NIL;
    } else if (v8obj->IsBoolean()) {
        mo->type = MSGPACK_OBJECT_BOOLEAN;
        mo->via.boolean = v8obj->BooleanValue();
    } else if (v8obj->IsNumber()) {
        double d = v8obj->NumberValue();
        if (trunc(d) != d) {
            mo->type = MSGPACK_OBJECT_DOUBLE;
            mo->via.dec = d;
        } else if (d > 0) {
            mo->type = MSGPACK_OBJECT_POSITIVE_INTEGER;
            mo->via.u64 = d;
        } else {
            mo->type = MSGPACK_OBJECT_NEGATIVE_INTEGER;
            mo->via.i64 = d;
        }
    } else if (v8obj->IsString()) {
        mo->type = MSGPACK_OBJECT_RAW;
        String::Utf8Value utf8value(v8obj);
        mo->via.raw.size = utf8value.length();
        mo->via.raw.ptr = (char*) msgpack_zone_malloc(mz, mo->via.raw.size);

        memcpy((char*) mo->via.raw.ptr, *utf8value, mo->via.raw.size);
    } else if (v8obj->IsArray()) {
        mc->enter(v8obj);

        Handle<Array> a = Handle<Array>::Cast(v8obj);

        mo->type = MSGPACK_OBJECT_ARRAY;
        mo->via.array.size = a->Length();
        mo->via.array.ptr = (msgpack_object*) msgpack_zone_malloc(
            mz,
            sizeof(msgpack_object) * mo->via.array.size
        );

        for (uint32_t i = 0, l = a->Length(); i < l; i++) {
            v8_to_msgpack(a->Get(i), &mo->via.array.ptr[i], mz, mc);
        }

        mc->out();
    } else if (Buffer::HasInstance(v8obj)) {
        Handle<Object> buf = Handle<Object>::Cast(v8obj);

        mo->type = MSGPACK_OBJECT_RAW;
        mo->via.raw.size = Buffer::Length(buf);
        mo->via.raw.ptr = Buffer::Data(buf);
    } else {
        mc->enter(v8obj);

        Handle<Object> o = Handle<Object>::Cast(v8obj);
        Local<Array> a = o->GetPropertyNames();

        mo->type = MSGPACK_OBJECT_MAP;
        mo->via.map.size = a->Length();
        mo->via.map.ptr = (msgpack_object_kv*) msgpack_zone_malloc(
            mz,
            sizeof(msgpack_object_kv) * mo->via.map.size
        );

        for (uint32_t i = 0, l = a->Length(); i < l; i++) {
            Local<Value> k = a->Get(i);

            v8_to_msgpack(k, &mo->via.map.ptr[i].key, mz, mc);
            v8_to_msgpack(o->Get(k), &mo->via.map.ptr[i].val, mz, mc);
        }

        mc->out();
    }
}

// Convert a MessagePack object to a V8 object.
//
// This method is recursive. It will probably blow out the stack on objects
// with extremely deep nesting.
static Handle<Value>
msgpack_to_v8(msgpack_object *mo) {
    switch (mo->type) {
    case MSGPACK_OBJECT_NIL:
        return Null();

    case MSGPACK_OBJECT_BOOLEAN:
        return (mo->via.boolean) ?
            True() :
            False();

    case MSGPACK_OBJECT_POSITIVE_INTEGER:
        return Integer::NewFromUnsigned(mo->via.u64);

    case MSGPACK_OBJECT_NEGATIVE_INTEGER:
        return Integer::New(mo->via.i64);

    case MSGPACK_OBJECT_DOUBLE:
        return Number::New(mo->via.dec);

    case MSGPACK_OBJECT_ARRAY: {
        Local<Array> a = Array::New(mo->via.array.size);

        for (uint32_t i = 0; i < mo->via.array.size; i++) {
            a->Set(i, msgpack_to_v8(&mo->via.array.ptr[i]));
        }

        return a;
    }

    case MSGPACK_OBJECT_RAW:
        return String::New(mo->via.raw.ptr, mo->via.raw.size);

    case MSGPACK_OBJECT_MAP: {
        Local<Object> o = Object::New();

        for (uint32_t i = 0; i < mo->via.map.size; i++) {
            o->Set(
                msgpack_to_v8(&mo->via.map.ptr[i].key),
                msgpack_to_v8(&mo->via.map.ptr[i].val)
            );
        }

        return o;
    }

    default:
        throw MsgpackException("Encountered unknown MesssagePack object type");
    }
}

// var buf = msgpack.pack(obj[, obj ...]);
//
// Returns a Buffer object representing the serialized state of the provided
// JavaScript object. If more arguments are provided, their serialized state
// will be accumulated to the end of the previous value(s).
//
// Any number of objects can be provided as arguments, and all will be
// serialized to the same bytestream, back-ty-back.
static Handle<Value>
pack(const Arguments &args) {
    HandleScope scope;

    msgpack_packer pk;
    MsgpackZone mz;
    MsgpackSbuffer sb;
    MsgpackCycle mc;

    msgpack_packer_init(&pk, &sb._sbuf, msgpack_sbuffer_write);

    for (int i = 0; i < args.Length(); i++) {
        msgpack_object mo;

        try {
            v8_to_msgpack(args[0], &mo, &mz._mz, &mc);
        } catch (MsgpackException e) {
            return ThrowException(e.getThrownException());
        }

        if (msgpack_pack_object(&pk, mo)) {
            return ThrowException(Exception::Error(
                String::New("Error serializaing object")));
        }
    }

    Buffer *bp = Buffer::New(sb._sbuf.data, sb._sbuf.size);

    return scope.Close(bp->handle_);
}

// var o = msgpack.unpack(buf);
//
// Return the JavaScript object resulting from unpacking the contents of the
// specified buffer. If the buffer does not contain a complete object, the
// undefined value is returned.
static Handle<Value>
unpack(const Arguments &args) {
    static Persistent<String> msgpack_bytes_remaining_symbol = 
        NODE_PSYMBOL("bytes_remaining");

    HandleScope scope;

    if (args.Length() < 0 || !Buffer::HasInstance(args[0])) {
        return ThrowException(Exception::TypeError(
            String::New("First argument must be a Buffer")));
    }

    Handle<Object> buf = args[0]->ToObject();

    MsgpackZone mz;
    msgpack_object mo;
    size_t off = 0;

    switch (msgpack_unpack(Buffer::Data(buf), Buffer::Length(buf), &off, &mz._mz, &mo)) {
    case MSGPACK_UNPACK_EXTRA_BYTES:
    case MSGPACK_UNPACK_SUCCESS:
        try {
            msgpack_unpack_template->GetFunction()->Set(
                msgpack_bytes_remaining_symbol,
                Integer::New(Buffer::Length(buf) - off)
            );
            return scope.Close(msgpack_to_v8(&mo));
        } catch (MsgpackException e) {
            return ThrowException(e.getThrownException());
        }
    
    case MSGPACK_UNPACK_CONTINUE:
        return scope.Close(Undefined());

    default:
        return ThrowException(Exception::Error(
            String::New("Error de-serializing object")));
    }
}

extern "C" void
init(Handle<Object> target) {
    HandleScope scope;

    NODE_SET_METHOD(target, "pack", pack);

    // Go through this mess rather than call NODE_SET_METHOD so that we can set
    // a field on the function for 'bytes_remaining'.
    msgpack_unpack_template = Persistent<FunctionTemplate>::New(
        FunctionTemplate::New(unpack)
    );
    target->Set(
        String::NewSymbol("unpack"),
        msgpack_unpack_template->GetFunction()
    );
}

// vim:ts=4 sw=4 et
