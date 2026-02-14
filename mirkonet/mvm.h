#pragma once
#include "config.h"
#include "types.h"

// 5-bit opcode encoding: [5-bit opcode | 3-bit modifier]
#define OP5_HALT      0x00
#define OP5_PUSH      0x01
#define OP5_POP       0x02
#define OP5_DUP       0x03
#define OP5_SWAP      0x04
#define OP5_ADD       0x05
#define OP5_SUB       0x06
#define OP5_MUL       0x07
#define OP5_DIV       0x08
#define OP5_MOD       0x09
#define OP5_AND       0x0A
#define OP5_OR        0x0B
#define OP5_XOR       0x0C
#define OP5_SHL       0x0D
#define OP5_SHR       0x0E
#define OP5_CMP       0x0F
#define OP5_NOT       0x10
#define OP5_JMP       0x11
#define OP5_SLOAD     0x12
#define OP5_SSTORE    0x13
#define OP5_EMIT      0x14
#define OP5_ARG       0x15
#define OP5_BALANCE   0x16
#define OP5_CALLVALUE 0x17
#define OP5_SHA3      0x18
// 0x19-0x1F reserved

#define ENCODE(op, mod)  (uint8_t)(((op) << 3) | ((mod) & 0x07))
#define DECODE_OP(b)     ((b) >> 3)
#define DECODE_MOD(b)    ((b) & 0x07)

#define BYTE_HALT      ENCODE(OP5_HALT, 0)
#define BYTE_REVERT    ENCODE(OP5_HALT, 1)
#define BYTE_PUSH8     ENCODE(OP5_PUSH, 1)
#define BYTE_PUSH16    ENCODE(OP5_PUSH, 2)
#define BYTE_PUSH32    ENCODE(OP5_PUSH, 3)
#define BYTE_POP       ENCODE(OP5_POP, 0)
#define BYTE_DUP       ENCODE(OP5_DUP, 0)
#define BYTE_SWAP      ENCODE(OP5_SWAP, 0)
#define BYTE_ADD       ENCODE(OP5_ADD, 0)
#define BYTE_SUB       ENCODE(OP5_SUB, 0)
#define BYTE_MUL       ENCODE(OP5_MUL, 0)
#define BYTE_DIV       ENCODE(OP5_DIV, 0)
#define BYTE_MOD       ENCODE(OP5_MOD, 0)
#define BYTE_AND       ENCODE(OP5_AND, 0)
#define BYTE_OR        ENCODE(OP5_OR, 0)
#define BYTE_XOR       ENCODE(OP5_XOR, 0)
#define BYTE_SHL       ENCODE(OP5_SHL, 0)
#define BYTE_SHR       ENCODE(OP5_SHR, 0)
#define BYTE_EQ        ENCODE(OP5_CMP, 0)
#define BYTE_GT        ENCODE(OP5_CMP, 1)
#define BYTE_LT        ENCODE(OP5_CMP, 2)
#define BYTE_NOT       ENCODE(OP5_NOT, 0)
#define BYTE_JUMP(t)   ENCODE(OP5_JMP, 0), (uint8_t)(t)
#define BYTE_JUMPI(t)  ENCODE(OP5_JMP, 1), (uint8_t)(t)
#define BYTE_SLOAD     ENCODE(OP5_SLOAD, 0)
#define BYTE_SSTORE    ENCODE(OP5_SSTORE, 0)
#define BYTE_EMIT      ENCODE(OP5_EMIT, 0)
#define BYTE_ARG(n)    ENCODE(OP5_ARG, (n))
#define BYTE_CALLER    ENCODE(OP5_ARG, 7)
#define BYTE_BALANCE   ENCODE(OP5_BALANCE, 0)
#define BYTE_CALLVALUE ENCODE(OP5_CALLVALUE, 0)
#define BYTE_SHA3      ENCODE(OP5_SHA3, 0)

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
    bool         tempCode;     // true = code fetched temporarily for validation, not permanently hosted

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


    // Cache bytecode temporarily for validation (non-host nodes).
    // Hash is verified against the stored codeHash from deployment.
    bool cacheCode(const uint8_t* bytecode, uint16_t len, bool temporary = false) {
        Hash256 h = sha256(bytecode, len);
        if (h != codeHash) return false;
        memcpy(code, bytecode, len);
        codeLen = len;
        hasCode = true;
        tempCode = temporary;
        return true;
    }

    // Discard temporarily cached bytecode after validation.
    // Only clears code for non-host nodes that fetched code for validation.
    void discardTempCode() {
        if (!tempCode) return;
        memset(code, 0, MVM_MAX_CODE);
        hasCode = false;
        tempCode = false;
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

            case OP5_HALT:
                _status = (mod == 0) ? VM_HALTED : VM_REVERTED;
                break;

            case OP5_PUSH:
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
                } else if (mod == 3) {
                    if (_pc+4 >= contract.codeLen) { _status = VM_ERR_CODE_BOUNDS; break; }
                    _stack[_sp++] = ((uint32_t)contract.code[_pc+1] << 24) |
                                    ((uint32_t)contract.code[_pc+2] << 16) |
                                    ((uint32_t)contract.code[_pc+3] << 8)  |
                                     contract.code[_pc+4];
                    _pc += 5;
                } else {
                    _status = VM_ERR_CODE_BOUNDS; break;
                }
                break;

            case OP5_POP:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                _sp--; _pc++; break;

            case OP5_DUP:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                if (_sp >= MVM_MAX_STACK) { _status = VM_ERR_STACK_OVER; break; }
                _stack[_sp] = _stack[_sp-1]; _sp++; _pc++; break;

            case OP5_SWAP:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                { uint32_t t = _stack[_sp-1]; _stack[_sp-1] = _stack[_sp-2];
                  _stack[_sp-2] = t; }
                _pc++; break;

            case OP5_ADD:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] += _stack[_sp-1]; _sp--; _pc++; break;

            case OP5_SUB:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] -= _stack[_sp-1]; _sp--; _pc++; break;

            case OP5_MUL:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] *= _stack[_sp-1]; _sp--; _pc++; break;

            case OP5_DIV:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                if (_stack[_sp-1] == 0) { _status = VM_ERR_DIV_ZERO; break; }
                _stack[_sp-2] /= _stack[_sp-1]; _sp--; _pc++; break;

            case OP5_MOD:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                if (_stack[_sp-1] == 0) { _status = VM_ERR_DIV_ZERO; break; }
                _stack[_sp-2] %= _stack[_sp-1]; _sp--; _pc++; break;

            case OP5_AND:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] &= _stack[_sp-1]; _sp--; _pc++; break;

            case OP5_OR:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] |= _stack[_sp-1]; _sp--; _pc++; break;

            case OP5_XOR:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] ^= _stack[_sp-1]; _sp--; _pc++; break;

            case OP5_SHL:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] <<= (_stack[_sp-1] & 31); _sp--; _pc++; break;

            case OP5_SHR:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-2] >>= (_stack[_sp-1] & 31); _sp--; _pc++; break;

            case OP5_CMP:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                if (mod == 0)
                    _stack[_sp-2] = (_stack[_sp-2] == _stack[_sp-1]) ? 1 : 0;
                else if (mod == 1)
                    _stack[_sp-2] = (_stack[_sp-2] > _stack[_sp-1]) ? 1 : 0;
                else
                    _stack[_sp-2] = (_stack[_sp-2] < _stack[_sp-1]) ? 1 : 0;
                _sp--; _pc++; break;

            case OP5_NOT:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-1] = (_stack[_sp-1] == 0) ? 1 : 0;
                _pc++; break;

            case OP5_JMP: {
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

            case OP5_SLOAD:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                _stack[_sp-1] = contract.storageGet(_stack[_sp-1]);
                _pc++; break;

            case OP5_SSTORE:
                if (_sp < 2) { _status = VM_ERR_STACK_UNDER; break; }
                { uint32_t v = _stack[--_sp]; uint32_t k = _stack[--_sp];
                  if (!contract.storageSet(k, v)) { _status = VM_ERR_STORAGE_FULL; break; } }
                _pc++; break;

            case OP5_EMIT:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                if (_eventCount < MVM_MAX_EVENTS)
                    _events[_eventCount++] = { _stack[--_sp], _gasUsed };
                else _sp--;
                _pc++; break;

            case OP5_ARG:
                if (_sp >= MVM_MAX_STACK) { _status = VM_ERR_STACK_OVER; break; }
                if (mod == 7) {
                    _stack[_sp++] = ctx.caller.hash32();
                } else {
                    if (mod >= ctx.argCount) { _status = VM_ERR_BAD_ARG; break; }
                    _stack[_sp++] = ctx.args[mod];
                }
                _pc++; break;

            case OP5_BALANCE:
                if (_sp >= MVM_MAX_STACK) { _status = VM_ERR_STACK_OVER; break; }
                _stack[_sp++] = contract.balance;
                _pc++; break;

            case OP5_CALLVALUE:
                if (_sp >= MVM_MAX_STACK) { _status = VM_ERR_STACK_OVER; break; }
                _stack[_sp++] = ctx.callValue;
                _pc++; break;

            case OP5_SHA3:
                if (_sp == 0) { _status = VM_ERR_STACK_UNDER; break; }
                {
                    uint32_t val = _stack[_sp-1];
                    uint8_t buf[4];
                    buf[0] = (val >> 24) & 0xFF;
                    buf[1] = (val >> 16) & 0xFF;
                    buf[2] = (val >> 8) & 0xFF;
                    buf[3] = val & 0xFF;
                    Hash256 h = sha256(buf, 4);
                    _stack[_sp-1] = ((uint32_t)h.bytes[0] << 24) |
                                    ((uint32_t)h.bytes[1] << 16) |
                                    ((uint32_t)h.bytes[2] << 8) |
                                     h.bytes[3];
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
