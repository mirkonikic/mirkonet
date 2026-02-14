#pragma once
#include "config.h"
#include "types.h"


#define OP4_HALT   0x0
#define OP4_PUSH   0x1
#define OP4_POP    0x2
#define OP4_DUP    0x3
#define OP4_SWAP   0x4
#define OP4_ADD    0x5
#define OP4_SUB    0x6
#define OP4_MUL    0x7
#define OP4_CMP    0x8
#define OP4_NOT    0x9
#define OP4_JMP    0xA
#define OP4_MOD    0xB
#define OP4_SLOAD  0xC
#define OP4_SSTORE 0xD
#define OP4_EMIT   0xE
#define OP4_ARG    0xF

#define ENCODE(op, mod)  (uint8_t)(((op) << 4) | ((mod) & 0x0F))
#define DECODE_OP(b)     ((b) >> 4)
#define DECODE_MOD(b)    ((b) & 0x0F)

#define BYTE_HALT      ENCODE(OP4_HALT, 0)
#define BYTE_REVERT    ENCODE(OP4_HALT, 1)
#define BYTE_PUSH8     ENCODE(OP4_PUSH, 1)
#define BYTE_PUSH16    ENCODE(OP4_PUSH, 2)
#define BYTE_POP       ENCODE(OP4_POP, 0)
#define BYTE_DUP       ENCODE(OP4_DUP, 0)
#define BYTE_SWAP      ENCODE(OP4_SWAP, 0)
#define BYTE_ADD       ENCODE(OP4_ADD, 0)
#define BYTE_SUB       ENCODE(OP4_SUB, 0)
#define BYTE_MUL       ENCODE(OP4_MUL, 0)
#define BYTE_EQ        ENCODE(OP4_CMP, 0)
#define BYTE_GT        ENCODE(OP4_CMP, 1)
#define BYTE_NOT       ENCODE(OP4_NOT, 0)
#define BYTE_JUMP(t)   ENCODE(OP4_JMP, 0), (uint8_t)(t)
#define BYTE_JUMPI(t)  ENCODE(OP4_JMP, 1), (uint8_t)(t)
#define BYTE_MOD       ENCODE(OP4_MOD, 0)
#define BYTE_SLOAD     ENCODE(OP4_SLOAD, 0)
#define BYTE_SSTORE    ENCODE(OP4_SSTORE, 0)
#define BYTE_EMIT      ENCODE(OP4_EMIT, 0)
#define BYTE_ARG(n)    ENCODE(OP4_ARG, (n))
#define BYTE_CALLER    ENCODE(OP4_ARG, 15)

enum MVMStatus : uint8_t {
    VM_RUNNING=0, VM_HALTED=1, VM_REVERTED=2,
    VM_ERR_STACK_OVER=3, VM_ERR_STACK_UNDER=4, VM_ERR_OUT_OF_GAS=5,
    VM_ERR_BAD_JUMP=6, VM_ERR_CODE_BOUNDS=7, VM_ERR_DIV_ZERO=8,
    VM_ERR_STORAGE_FULL=9, VM_ERR_BAD_ARG=10,
};

inline const char* statusName(MVMStatus s) {
    const char* names[] = {"RUNNING","OK","REVERTED","STACK_OVER","STACK_UNDER",
        "OUT_OF_GAS","BAD_JUMP","CODE_BOUNDS","DIV_ZERO","STORAGE_FULL","BAD_ARG"};
    return (s <= VM_ERR_BAD_ARG) ? names[s] : "UNKNOWN";
}
inline bool isTerminal(MVMStatus s) { return s != VM_RUNNING; }

struct MVMEvent { uint32_t value; uint32_t gasUsed; };
struct StorageSlot { uint32_t key; uint32_t value; bool used; };


struct Contract {
    char         name[16];
    ContractAddr addr;
    NodeID       deployer;
    NodeID       host;
    Hash256      codeHash;
    uint8_t      code[MVM_MAX_CODE];
    uint16_t     codeLen;
    StorageSlot  storage[MVM_MAX_STORAGE];
    uint32_t     balance;
    bool         active;
    bool         hasCode;

    void init(const char* n, const uint8_t* bytecode, uint16_t len,
              const NodeID& dep, bool isHost) {
        memset(this, 0, sizeof(Contract));
        strncpy(name, n, 15);
        addr = ContractAddr::fromName(n);
        deployer = dep;
        host = dep;
        if (bytecode && len > 0) {
            memcpy(code, bytecode, min((int)len, (int)MVM_MAX_CODE));
            codeLen = min(len, (uint16_t)MVM_MAX_CODE);
            codeHash = sha256(code, codeLen);
            hasCode = isHost;
        }
        active = true;
    }


    void initRemote(const char* n, const NodeID& dep,
                    const NodeID& hostNode, const Hash256& hash) {
        memset(this, 0, sizeof(Contract));
        strncpy(name, n, 15);
        addr = ContractAddr::fromName(n);
        deployer = dep;
        host = hostNode;
        codeHash = hash;
        codeLen = 0;
        hasCode = false;
        active = true;
    }


    bool cacheCode(const uint8_t* bytecode, uint16_t len) {
        Hash256 h = sha256(bytecode, len);
        if (h != codeHash) return false;
        memcpy(code, bytecode, len);
        codeLen = len;
        hasCode = true;
        return true;
    }

    uint32_t storageGet(uint32_t key) const {
        for (int i = 0; i < MVM_MAX_STORAGE; i++)
            if (storage[i].used && storage[i].key == key) return storage[i].value;
        return 0;
    }

    bool storageSet(uint32_t key, uint32_t value) {
        for (int i = 0; i < MVM_MAX_STORAGE; i++) {
            if (storage[i].used && storage[i].key == key) {
                storage[i].value = value; return true;
            }
        }
        for (int i = 0; i < MVM_MAX_STORAGE; i++) {
            if (!storage[i].used) { storage[i] = {key, value, true}; return true; }
        }
        return false;
    }

    Hash256 stateHash() const {
        uint8_t buf[MVM_MAX_STORAGE * 8 + 16];
        size_t off = 0;
        memcpy(buf + off, addr.bytes, 4); off += 4;
        memcpy(buf + off, &balance, 4); off += 4;
        for (int i = 0; i < MVM_MAX_STORAGE; i++) {
            if (storage[i].used) {
                memcpy(buf + off, &storage[i].key, 4); off += 4;
                memcpy(buf + off, &storage[i].value, 4); off += 4;
            }
        }
        return sha256(buf, off);
    }
};

struct MVMContext {
    NodeID   caller;
    uint32_t args[MVM_MAX_ARGS];
    uint8_t  argCount;
    uint32_t callValue;
};


class MirkoVM {
public:
    MVMStatus execute(Contract& contract, const MVMContext& ctx) {
        _pc = 0; _sp = 0; _gas = MVM_MAX_GAS; _gasUsed = 0;
        _status = VM_RUNNING; _eventCount = 0;
        memcpy(_snap, contract.storage, sizeof(contract.storage));
        uint32_t balSnap = contract.balance;

        while (_status == VM_RUNNING) {
            if (_gas == 0) { _status = VM_ERR_OUT_OF_GAS; break; }
            if (_pc >= contract.codeLen) { _status = VM_ERR_CODE_BOUNDS; break; }
            _gas--; _gasUsed++;

            uint8_t byte = contract.code[_pc];
            uint8_t op  = DECODE_OP(byte);
            uint8_t mod = DECODE_MOD(byte);

            switch (op) {

            case OP4_HALT:
                _status = (mod == 0) ? VM_HALTED : VM_REVERTED;
                break;

            case OP4_PUSH:
                if (_sp >= MVM_MAX_STACK) { _status = VM_ERR_STACK_OVER; break; }
                if (mod == 1) {

                    if (_pc+1 >= contract.codeLen) { _status = VM_ERR_CODE_BOUNDS; break; }
                    _stack[_sp++] = contract.code[_pc+1];
                    _pc += 2;
                } else if (mod == 2) {

                    if (_pc+2 >= contract.codeLen) { _status = VM_ERR_CODE_BOUNDS; break; }
                    _stack[_sp++] = ((uint32_t)contract.code[_pc+1] << 8) |
                                     contract.code[_pc+2];
                    _pc += 3;
                } else {
                    _status = VM_ERR_CODE_BOUNDS; break;
                }
                break;

            case OP4_POP:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                _sp--; _pc++; break;

            case OP4_DUP:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                if (_sp >= MVM_MAX_STACK) { _status = VM_ERR_STACK_OVER; break; }
                _stack[_sp] = _stack[_sp-1]; _sp++; _pc++; break;

            case OP4_SWAP:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                { uint32_t t = _stack[_sp-1]; _stack[_sp-1] = _stack[_sp-2];
                  _stack[_sp-2] = t; }
                _pc++; break;

            case OP4_ADD:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] += _stack[_sp-1]; _sp--; _pc++; break;

            case OP4_SUB:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] -= _stack[_sp-1]; _sp--; _pc++; break;

            case OP4_MUL:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] *= _stack[_sp-1]; _sp--; _pc++; break;

            case OP4_CMP:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                if (mod == 0)
                    _stack[_sp-2] = (_stack[_sp-2] == _stack[_sp-1]) ? 1 : 0;
                else
                    _stack[_sp-2] = (_stack[_sp-2] > _stack[_sp-1]) ? 1 : 0;
                _sp--; _pc++; break;

            case OP4_NOT:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-1] = (_stack[_sp-1] == 0) ? 1 : 0;
                _pc++; break;

            case OP4_JMP: {
                if (_pc+1 >= contract.codeLen) { _status = VM_ERR_CODE_BOUNDS; break; }
                uint8_t target = contract.code[_pc+1];
                if (target >= contract.codeLen) { _status = VM_ERR_BAD_JUMP; break; }
                if (mod == 0) {

                    _pc = target;
                } else {

                    if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                    uint32_t cond = _stack[--_sp];
                    _pc = cond ? target : (_pc + 2);
                }
                break;
            }

            case OP4_MOD:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                if (_stack[_sp-1] == 0) { _status = VM_ERR_DIV_ZERO; break; }
                _stack[_sp-2] %= _stack[_sp-1]; _sp--; _pc++; break;

            case OP4_SLOAD:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-1] = contract.storageGet(_stack[_sp-1]);
                _pc++; break;

            case OP4_SSTORE:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                { uint32_t v = _stack[--_sp]; uint32_t k = _stack[--_sp];
                  if (!contract.storageSet(k, v)) { _status = VM_ERR_STORAGE_FULL; break; } }
                _pc++; break;

            case OP4_EMIT:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                if (_eventCount < MVM_MAX_EVENTS)
                    _events[_eventCount++] = { _stack[--_sp], _gasUsed };
                else _sp--;
                _pc++; break;

            case OP4_ARG:
                if (_sp >= MVM_MAX_STACK) { _status = VM_ERR_STACK_OVER; break; }
                if (mod == 15) {

                    _stack[_sp++] = ctx.caller.hash32();
                } else {

                    if (mod >= ctx.argCount) { _status = VM_ERR_BAD_ARG; break; }
                    _stack[_sp++] = ctx.args[mod];
                }
                _pc++; break;

            default:
                _status = VM_ERR_CODE_BOUNDS; break;
            }
        }


        if (_status != VM_HALTED) {
            memcpy(contract.storage, _snap, sizeof(contract.storage));
            contract.balance = balSnap;
        }
        return _status;
    }

    uint32_t gasUsed() const { return _gasUsed; }
    MVMStatus status() const { return _status; }
    uint32_t stackTop() const { return _sp > 0 ? _stack[_sp-1] : 0; }
    uint8_t eventCount() const { return _eventCount; }
    const MVMEvent& event(uint8_t i) const { return _events[i]; }

private:
    uint32_t    _stack[MVM_MAX_STACK];
    uint8_t     _sp;
    uint32_t    _pc, _gas, _gasUsed;
    MVMStatus   _status;
    MVMEvent    _events[MVM_MAX_EVENTS];
    uint8_t     _eventCount;
    StorageSlot _snap[MVM_MAX_STORAGE];
};