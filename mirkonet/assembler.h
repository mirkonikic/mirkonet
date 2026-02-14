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
            if (off >= MVM_MAX_CODE - 3) {
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


            if      (mn == "HALT")   r.code[off++] = BYTE_HALT;
            else if (mn == "REVERT") r.code[off++] = BYTE_REVERT;
            else if (mn == "POP")    r.code[off++] = BYTE_POP;
            else if (mn == "DUP")    r.code[off++] = BYTE_DUP;
            else if (mn == "SWAP")   r.code[off++] = BYTE_SWAP;
            else if (mn == "ADD")    r.code[off++] = BYTE_ADD;
            else if (mn == "SUB")    r.code[off++] = BYTE_SUB;
            else if (mn == "MUL")    r.code[off++] = BYTE_MUL;
            else if (mn == "MOD")    r.code[off++] = BYTE_MOD;
            else if (mn == "EQ")     r.code[off++] = BYTE_EQ;
            else if (mn == "GT")     r.code[off++] = ENCODE(OP4_CMP, 1);
            else if (mn == "NOT")    r.code[off++] = BYTE_NOT;
            else if (mn == "SLOAD")  r.code[off++] = BYTE_SLOAD;
            else if (mn == "SSTORE") r.code[off++] = BYTE_SSTORE;
            else if (mn == "EMIT")   r.code[off++] = BYTE_EMIT;
            else if (mn == "CALLER") r.code[off++] = BYTE_CALLER;


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
                } else {
                    r.code[off++] = BYTE_PUSH16;
                    r.code[off++] = (uint8_t)(val >> 8);
                    r.code[off++] = (uint8_t)(val & 0xFF);
                }
            }


            else if (mn == "JUMP") {
                uint8_t target = resolveTarget(operand, labels, r, lineNum);
                if (!r.ok) return r;
                r.code[off++] = ENCODE(OP4_JMP, 0);
                r.code[off++] = target;
            }


            else if (mn == "JUMPI") {
                uint8_t target = resolveTarget(operand, labels, r, lineNum);
                if (!r.ok) return r;
                r.code[off++] = ENCODE(OP4_JMP, 1);
                r.code[off++] = target;
            }


            else if (mn == "ARG") {
                uint8_t idx = (uint8_t)operand.toInt();
                if (idx > 14) {
                    r.ok = false;
                    r.error = "ARG index 0-14 only, L" + String(lineNum);
                    return r;
                }
                r.code[off++] = ENCODE(OP4_ARG, idx);
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
            case OP4_HALT:
                out += (mod == 0) ? "HALT\n" : "REVERT\n"; pc++; break;
            case OP4_PUSH:
                if (mod == 1 && pc+1 < len) {
                    out += "PUSH " + String(code[pc+1]) + "\n"; pc += 2;
                } else if (mod == 2 && pc+2 < len) {
                    uint16_t v = ((uint16_t)code[pc+1] << 8) | code[pc+2];
                    out += "PUSH " + String(v) + "\n"; pc += 3;
                } else { out += "PUSH ??\n"; pc++; }
                break;
            case OP4_POP:    out += "POP\n"; pc++; break;
            case OP4_DUP:    out += "DUP\n"; pc++; break;
            case OP4_SWAP:   out += "SWAP\n"; pc++; break;
            case OP4_ADD:    out += "ADD\n"; pc++; break;
            case OP4_SUB:    out += "SUB\n"; pc++; break;
            case OP4_MUL:    out += "MUL\n"; pc++; break;
            case OP4_CMP:    out += (mod==0) ? "EQ\n" : "GT\n"; pc++; break;
            case OP4_NOT:    out += "NOT\n"; pc++; break;
            case OP4_JMP:
                if (pc+1 < len) {
                    out += (mod==0 ? "JUMP " : "JUMPI ") + String(code[pc+1]) + "\n";
                    pc += 2;
                } else { out += "JMP ??\n"; pc++; }
                break;
            case OP4_MOD:    out += "MOD\n"; pc++; break;
            case OP4_SLOAD:  out += "SLOAD\n"; pc++; break;
            case OP4_SSTORE: out += "SSTORE\n"; pc++; break;
            case OP4_EMIT:   out += "EMIT\n"; pc++; break;
            case OP4_ARG:
                if (mod == 15) out += "CALLER\n";
                else out += "ARG " + String(mod) + "\n";
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
            return (val <= 255) ? 2 : 3;
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