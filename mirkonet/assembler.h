#pragma once
#include "mvm.h"
#include <map>

class MVMAssembler {
public:
    struct Result {
        uint8_t  code[MVM_MAX_CODE];
        uint16_t len;
        bool     ok;
        String   error;
    };

    static Result assemble(const String& source) {
        Result r; r.len = 0; r.ok = true; r.error = "";
        memset(r.code, 0, MVM_MAX_CODE);
        std::map<String, uint16_t> labels;


        uint16_t off = 0;
        int pos = 0;
        String src = source;
        while (pos <= (int)src.length()) {
            int end = src.indexOf('\n', pos);
            if (end < 0) end = src.length();
            String line = src.substring(pos, end); line.trim();
            pos = end + 1;
            if (line.length() == 0 || line.startsWith(";") || line.startsWith("#"))
                continue;
            if (line.endsWith(":")) {
                String l = line.substring(0, line.length()-1); l.trim();
                labels[l] = off;
                continue;
            }
            String mn = line;
            int sp = line.indexOf(' ');
            String operand = "";
            if (sp > 0) {
                mn = line.substring(0, sp);
                operand = line.substring(sp+1); operand.trim();
            }
            mn.toUpperCase();
            off += instrSize(mn, operand);
        }


        off = 0; pos = 0; int lineNum = 0;
        while (pos <= (int)src.length()) {
            int end = src.indexOf('\n', pos);
            if (end < 0) end = src.length();
            String line = src.substring(pos, end); line.trim();
            pos = end + 1; lineNum++;
            if (line.length() == 0 || line.startsWith(";") || line.startsWith("#"))
                continue;
            if (line.endsWith(":")) continue;
            if (off >= MVM_MAX_CODE - 5) {
                r.ok = false;
                r.error = "Code too long L" + String(lineNum);
                return r;
            }

            String mn = line, operand = "";
            int sp = line.indexOf(' ');
            if (sp > 0) {
                mn = line.substring(0, sp);
                operand = line.substring(sp+1); operand.trim();
            }
            mn.toUpperCase();


            if      (mn == "HALT")      r.code[off++] = BYTE_HALT;
            else if (mn == "REVERT")    r.code[off++] = BYTE_REVERT;
            else if (mn == "POP")       r.code[off++] = BYTE_POP;
            else if (mn == "DUP")       r.code[off++] = BYTE_DUP;
            else if (mn == "SWAP")      r.code[off++] = BYTE_SWAP;
            else if (mn == "ADD")       r.code[off++] = BYTE_ADD;
            else if (mn == "SUB")       r.code[off++] = BYTE_SUB;
            else if (mn == "MUL")       r.code[off++] = BYTE_MUL;
            else if (mn == "DIV")       r.code[off++] = BYTE_DIV;
            else if (mn == "MOD")       r.code[off++] = BYTE_MOD;
            else if (mn == "AND")       r.code[off++] = BYTE_AND;
            else if (mn == "OR")        r.code[off++] = BYTE_OR;
            else if (mn == "XOR")       r.code[off++] = BYTE_XOR;
            else if (mn == "SHL")       r.code[off++] = BYTE_SHL;
            else if (mn == "SHR")       r.code[off++] = BYTE_SHR;
            else if (mn == "EQ")        r.code[off++] = BYTE_EQ;
            else if (mn == "GT")        r.code[off++] = ENCODE(OP5_CMP, 1);
            else if (mn == "LT")        r.code[off++] = BYTE_LT;
            else if (mn == "NOT")       r.code[off++] = BYTE_NOT;
            else if (mn == "SLOAD")     r.code[off++] = BYTE_SLOAD;
            else if (mn == "SSTORE")    r.code[off++] = BYTE_SSTORE;
            else if (mn == "EMIT")      r.code[off++] = BYTE_EMIT;
            else if (mn == "CALLER")    r.code[off++] = BYTE_CALLER;
            else if (mn == "BALANCE")   r.code[off++] = BYTE_BALANCE;
            else if (mn == "CALLVALUE") r.code[off++] = BYTE_CALLVALUE;
            else if (mn == "SHA3")      r.code[off++] = BYTE_SHA3;
            // --- New opcodes ---
            else if (mn == "LOG0")      r.code[off++] = BYTE_LOG(0);
            else if (mn == "LOG1")      r.code[off++] = BYTE_LOG(1);
            else if (mn == "LOG2")      r.code[off++] = BYTE_LOG(2);
            else if (mn == "LOG3")      r.code[off++] = BYTE_LOG(3);
            else if (mn == "LOG4")      r.code[off++] = BYTE_LOG(4);
            else if (mn == "ISZERO")    r.code[off++] = BYTE_ISZERO;
            else if (mn == "MLOAD")     r.code[off++] = BYTE_MLOAD;
            else if (mn == "MSTORE")    r.code[off++] = BYTE_MSTORE;
            else if (mn == "ADDRESS")   r.code[off++] = BYTE_ADDRESS;
            else if (mn == "ORIGIN")    r.code[off++] = BYTE_ORIGIN;
            else if (mn == "NUMBER")    r.code[off++] = BYTE_NUMBER;
            else if (mn == "TIMESTAMP") r.code[off++] = BYTE_TIMESTAMP;
            else if (mn == "GASLEFT")   r.code[off++] = BYTE_GASLEFT;
            else if (mn == "CALL")      r.code[off++] = BYTE_CALL;
            else if (mn == "DELEGATECALL" || mn == "DCALL")
                                        r.code[off++] = BYTE_DCALL;


            else if (mn == "PUSH") {
                uint32_t val = 0;
                if (operand.startsWith("@")) {
                    String lbl = operand.substring(1);
                    if (labels.find(lbl) != labels.end()) val = labels[lbl];
                    else {
                        r.ok = false;
                        r.error = "Unknown label '" + lbl + "' L" + String(lineNum);
                        return r;
                    }
                } else if (operand.startsWith("0x")) {
                    val = strtoul(operand.c_str(), nullptr, 16);
                } else {
                    val = operand.toInt();
                }

                if (val <= 255) {
                    r.code[off++] = BYTE_PUSH8;
                    r.code[off++] = (uint8_t)val;
                } else if (val <= 65535) {
                    r.code[off++] = BYTE_PUSH16;
                    r.code[off++] = (uint8_t)(val >> 8);
                    r.code[off++] = (uint8_t)(val & 0xFF);
                } else {
                    r.code[off++] = BYTE_PUSH32;
                    r.code[off++] = (uint8_t)(val >> 24);
                    r.code[off++] = (uint8_t)(val >> 16);
                    r.code[off++] = (uint8_t)(val >> 8);
                    r.code[off++] = (uint8_t)(val & 0xFF);
                }
            }


            else if (mn == "JUMP") {
                uint8_t target = resolveTarget(operand, labels, r, lineNum);
                if (!r.ok) return r;
                r.code[off++] = ENCODE(OP5_JMP, 0);
                r.code[off++] = target;
            }


            else if (mn == "JUMPI") {
                uint8_t target = resolveTarget(operand, labels, r, lineNum);
                if (!r.ok) return r;
                r.code[off++] = ENCODE(OP5_JMP, 1);
                r.code[off++] = target;
            }


            else if (mn == "ARG") {
                uint8_t idx = (uint8_t)operand.toInt();
                if (idx > 6) {
                    r.ok = false;
                    r.error = "ARG index 0-6 only, L" + String(lineNum);
                    return r;
                }
                r.code[off++] = ENCODE(OP5_ARG, idx);
            }

            else {
                r.ok = false;
                r.error = "Unknown '" + mn + "' L" + String(lineNum);
                return r;
            }
        }

        r.len = off;
        return r;
    }



    static String disassemble(const uint8_t* code, uint16_t len) {
        String out; uint16_t pc = 0;
        while (pc < len) {
            out += String(pc) + ": ";
            uint8_t byte = code[pc];
            uint8_t op = DECODE_OP(byte);
            uint8_t mod = DECODE_MOD(byte);

            switch (op) {
            case OP5_HALT:
                out += (mod == 0) ? "HALT\n" : "REVERT\n"; pc++; break;
            case OP5_PUSH:
                if (mod == 1 && pc+1 < len) {
                    out += "PUSH " + String(code[pc+1]) + "\n"; pc += 2;
                } else if (mod == 2 && pc+2 < len) {
                    uint16_t v = ((uint16_t)code[pc+1] << 8) | code[pc+2];
                    out += "PUSH " + String(v) + "\n"; pc += 3;
                } else if (mod == 3 && pc+4 < len) {
                    uint32_t v = ((uint32_t)code[pc+1] << 24) |
                                 ((uint32_t)code[pc+2] << 16) |
                                 ((uint32_t)code[pc+3] << 8) |
                                  code[pc+4];
                    out += "PUSH " + String(v) + "\n"; pc += 5;
                } else { out += "PUSH ??\n"; pc++; }
                break;
            case OP5_POP:       out += "POP\n"; pc++; break;
            case OP5_DUP:       out += "DUP\n"; pc++; break;
            case OP5_SWAP:      out += "SWAP\n"; pc++; break;
            case OP5_ADD:       out += "ADD\n"; pc++; break;
            case OP5_SUB:       out += "SUB\n"; pc++; break;
            case OP5_MUL:       out += "MUL\n"; pc++; break;
            case OP5_DIV:       out += "DIV\n"; pc++; break;
            case OP5_MOD:       out += "MOD\n"; pc++; break;
            case OP5_AND:       out += "AND\n"; pc++; break;
            case OP5_OR:        out += "OR\n"; pc++; break;
            case OP5_XOR:       out += "XOR\n"; pc++; break;
            case OP5_SHL:       out += "SHL\n"; pc++; break;
            case OP5_SHR:       out += "SHR\n"; pc++; break;
            case OP5_CMP:
                if (mod == 0) out += "EQ\n";
                else if (mod == 1) out += "GT\n";
                else out += "LT\n";
                pc++; break;
            case OP5_NOT:       out += "NOT\n"; pc++; break;
            case OP5_JMP:
                if (pc+1 < len) {
                    out += (mod==0 ? "JUMP " : "JUMPI ") + String(code[pc+1]) + "\n";
                    pc += 2;
                } else { out += "JMP ??\n"; pc++; }
                break;
            case OP5_SLOAD:     out += "SLOAD\n"; pc++; break;
            case OP5_SSTORE:    out += "SSTORE\n"; pc++; break;
            case OP5_EMIT:      out += "EMIT\n"; pc++; break;
            case OP5_ARG:
                if (mod == 7) out += "CALLER\n";
                else out += "ARG " + String(mod) + "\n";
                pc++; break;
            case OP5_BALANCE:   out += "BALANCE\n"; pc++; break;
            case OP5_CALLVALUE: out += "CALLVALUE\n"; pc++; break;
            case OP5_SHA3:      out += "SHA3\n"; pc++; break;
            case OP5_LOG:
                out += "LOG" + String(mod) + "\n"; pc++; break;
            case OP5_EXT:
                switch (mod) {
                case EXT_ISZERO:    out += "ISZERO\n"; break;
                case EXT_MLOAD:     out += "MLOAD\n"; break;
                case EXT_MSTORE:    out += "MSTORE\n"; break;
                case EXT_ADDRESS:   out += "ADDRESS\n"; break;
                case EXT_ORIGIN:    out += "ORIGIN\n"; break;
                case EXT_NUMBER:    out += "NUMBER\n"; break;
                case EXT_TIMESTAMP: out += "TIMESTAMP\n"; break;
                case EXT_GASLEFT:   out += "GASLEFT\n"; break;
                default: out += "EXT?" + String(mod) + "\n"; break;
                }
                pc++; break;
            case OP5_XCALL:
                out += (mod == 0) ? "CALL\n" : "DELEGATECALL\n";
                pc++; break;
            default: out += "??(0x" + String(byte, HEX) + ")\n"; pc++; break;
            }
        }
        return out;
    }

private:
    static uint16_t instrSize(const String& mn, const String& operand) {
        if (mn == "PUSH") {
            if (operand.startsWith("@")) return 2;
            uint32_t val = 0;
            if (operand.startsWith("0x")) val = strtoul(operand.c_str(), nullptr, 16);
            else val = operand.toInt();
            if (val <= 255) return 2;
            if (val <= 65535) return 3;
            return 5;
        }
        if (mn == "JUMP" || mn == "JUMPI") return 2;
        return 1;
    }

    static uint8_t resolveTarget(const String& operand,
                                  const std::map<String, uint16_t>& labels,
                                  Result& r, int lineNum) {
        String lbl = operand;
        if (lbl.startsWith("@")) lbl = lbl.substring(1);
        auto it = labels.find(lbl);
        if (it != labels.end()) return (uint8_t)it->second;
        r.ok = false;
        r.error = "Unknown label '" + lbl + "' L" + String(lineNum);
        return 0;
    }
};
