// Native renderer (stage 1): the co-issue scan and HLSL patch of native_coissue.h, standard library only (the offline
// check in stage1/subagents/bridge.md compiles it on its own against the whole shader cache).
//
// Microcode layouts as the SDK's ucode.h (ControlFlowExecInstruction, AluInstruction; BSD-3, read only) and the fork's
// shader_code.h; the instruction walk follows the fork's ShaderRecompiler::recompile (control-flow pairs up to the
// lowest exec address, exec / cond_exec / cond_exec_pred[_clean] blocks in order, sequence bit 2k = fetch) and its ALU
// emission (one "ps = " line per scalar operation other than retain_prev, preceded by "p0 = ..." for setp; the vector
// write line right before the scalar part).
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace pdz::native::coissue
{
  // One ALU instruction whose scalar operation reads a temporary register component its vector operation writes.
  struct Hazard
  {
    uint32_t ordinal = 0;       // among the instructions with a scalar operation other than retain_prev
    uint32_t address = 0;       // instruction address in the microcode (log)
    uint32_t vector_dest = 0;   // r<n> written by the vector operation
    uint32_t vector_mask = 0;   // its write mask
    uint32_t vector_opcode = 0; // ucode AluVectorOpcode
    uint32_t scalar_opcode = 0; // ucode AluScalarOpcode
  };

  // An rcpf / rsqf scalar operation (native_hlslfix.h, pdz_native_rcpf_zero): the n-th "ps = " line of the fork's HLSL.
  struct ScalarSite
  {
    uint32_t ordinal = 0; // among the instructions with a scalar operation other than retain_prev
    uint32_t address = 0; // instruction address in the microcode (log)
    uint32_t opcode = 0;  // ucode AluScalarOpcode (rcpf 18, rsqf 21)
  };

  struct ScanResult
  {
    bool ok = false;         // the microcode was parsed
    uint32_t scalar_ops = 0; // instructions with a scalar operation other than retain_prev ("ps = " lines)
    std::vector<Hazard> hazards;
    std::vector<ScalarSite> rcpf_rsqf; // rcpf / rsqf operations, in "ps = " order
    std::string why; // parse failure
  };

  namespace scan_detail
  {
    constexpr uint32_t kMaxDwords = 16384;
    constexpr uint32_t kScalarRcpf = 18, kScalarRsqf = 21;
    constexpr uint32_t kScalarMaxAs = 23, kScalarMaxAsf = 24;
    constexpr uint32_t kScalarSetpFirst = 27, kScalarSetpLast = 34; // setp_eq .. setp_rstr
    constexpr uint32_t kScalarSetpClr = 33;
    constexpr uint32_t kScalarConstFirst = 42, kScalarConstLast = 47; // mulsc0 .. subsc1 (constant x temporary)
    constexpr uint32_t kScalarRetainPrev = 50;

    inline const char *VectorName(uint32_t op)
    {
      static const char *const kNames[32] = {
          "add",          "mul",          "max",          "min",          "seq",          "sgt",     "sge",
          "sne",          "frc",          "trunc",        "floor",        "mad",          "cndeq",   "cndge",
          "cndgt",        "dp4",          "dp3",          "dp2add",       "cube",         "max4",    "setp_eq_push",
          "setp_ne_push", "setp_gt_push", "setp_ge_push", "kill_eq",      "kill_gt",      "kill_ge", "kill_ne",
          "dst",          "maxa",         "op30",         "op31"};
      return kNames[op & 31];
    }

    inline const char *ScalarName(uint32_t op)
    {
      static const char *const kNames[51] = {
          "adds",      "adds_prev", "muls",    "muls_prev", "muls_prev2", "maxs",     "mins",     "seqs",
          "sgts",      "sges",      "snes",    "frcs",      "truncs",     "floors",   "exp",      "logc",
          "log",       "rcpc",      "rcpf",    "rcp",       "rsqc",       "rsqf",     "rsq",      "maxas",
          "maxasf",    "subs",      "subs_prev", "setp_eq", "setp_ne",    "setp_gt",  "setp_ge",  "setp_inv",
          "setp_pop",  "setp_clr",  "setp_rstr", "kills_eq", "kills_gt",  "kills_ge", "kills_ne", "kills_one",
          "sqrt",      "op41",      "mulsc0",  "mulsc1",    "addsc0",     "addsc1",   "subsc0",   "subsc1",
          "sin",       "cos",       "retain_prev"};
      return op < 51 ? kNames[op] : "op5x";
    }

    // Operand count of the vector operation (ucode kAluVectorOpcodeInfos): decides the scalar `b` component.
    inline uint32_t VectorOperandCount(uint32_t op)
    {
      switch (op)
      {
      case 8: case 9: case 10: case 19: return 1;            // frc, trunc, floor, max4
      case 11: case 12: case 13: case 14: case 17: return 3; // mad, cndeq, cndge, cndgt, dp2add
      case 30: case 31: return 0;
      default: return 2;
      }
    }

    // Scalar operations reading two components of source 3 (`a` = W, `b` = X, or Z after a 3-operand vector op).
    inline bool ScalarTwoComponent(uint32_t op)
    {
      return op == 0 || op == 2 || op == 4 || op == 5 || op == 6 || op == 23 || op == 24 || op == 25;
    }

    // Scalar operations without a source operand.
    inline bool ScalarNoSource(uint32_t op) { return op == kScalarSetpClr || op == 41 || op >= kScalarRetainPrev; }

    // Vector operations whose HLSL is the write line alone (setp_*_push, kill_* and maxa emit a second line).
    inline bool VectorOneLine(uint32_t op) { return !(op >= 20 && op <= 27) && op != 29; }

    inline uint32_t SwizzledComponent(uint32_t swizzle, uint32_t i) { return ((swizzle >> (2 * i)) + i) & 3; }

    inline uint32_t LoadBe(const uint8_t *p)
    {
      return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    inline std::string Trim(const std::string &s)
    {
      size_t b = 0, e = s.size();
      while (b < e && (s[b] == ' ' || s[b] == '\t'))
      {
        ++b;
      }
      while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r'))
      {
        --e;
      }
      return s.substr(b, e - b);
    }

    inline bool StartsWith(const std::string &s, const std::string &prefix) { return s.rfind(prefix, 0) == 0; }

    inline std::string At(uint32_t address) { return "@" + std::to_string(address) + ": "; }
  } // namespace scan_detail

  // `ucode_be`: the microcode dwords in guest (big-endian) order, `bytes` long.
  inline ScanResult Scan(const uint8_t *ucode_be, uint32_t bytes)
  {
    using namespace scan_detail;
    ScanResult r;
    if (!ucode_be || bytes < 12 || (bytes % 4) != 0)
    {
      r.why = "no microcode";
      return r;
    }
    const uint32_t dwords = std::min<uint32_t>(bytes / 4, kMaxDwords);
    auto W = [&](uint32_t i) { return LoadBe(ucode_be + 4 * i); };

    // Control-flow pairs (3 dwords each) up to the lowest exec address, as the fork's first pass.
    struct Exec
    {
      uint32_t address, count, sequence;
    };
    std::vector<Exec> execs;
    uint32_t limit = dwords / 3; // in pairs
    for (uint32_t pair = 0; pair < limit && pair * 3 + 2 < dwords; ++pair)
    {
      const uint32_t d0 = W(pair * 3), d1 = W(pair * 3 + 1), d2 = W(pair * 3 + 2);
      const uint64_t cfs[2] = {uint64_t(d0) | (uint64_t(d1 & 0xFFFFu) << 32), uint64_t(d1 >> 16) | (uint64_t(d2) << 16)};
      for (uint64_t cf : cfs)
      {
        const uint32_t op = uint32_t(cf >> 44) & 0xF;
        // exec, exec_end, cond_exec, cond_exec_end, cond_exec_pred, cond_exec_pred_end, cond_exec_pred_clean(_end)
        if ((op >= 1 && op <= 6) || op == 13 || op == 14)
        {
          const uint32_t address = uint32_t(cf) & 0xFFF;
          execs.push_back({address, uint32_t(cf >> 12) & 7, uint32_t(cf >> 16) & 0xFFF});
          if (address != 0)
          {
            limit = std::min(limit, address);
          }
        }
      }
    }

    uint32_t ordinal = 0;
    for (const Exec &e : execs)
    {
      uint32_t sequence = e.sequence;
      for (uint32_t k = 0; k < e.count; ++k, sequence >>= 2)
      {
        if (sequence & 1u)
        {
          continue; // fetch
        }
        const uint32_t at = e.address + k;
        if (at * 3 + 2 >= dwords)
        {
          r.why = "instruction " + std::to_string(at) + " past the microcode end";
          return r;
        }
        const uint32_t d0 = W(at * 3), d1 = W(at * 3 + 1), d2 = W(at * 3 + 2);
        const uint32_t scalar_op = d0 >> 26;
        if (scalar_op == kScalarRetainPrev)
        {
          continue; // the fork emits no "ps = " line
        }
        const uint32_t my_ordinal = ordinal++;
        if (scalar_op == kScalarRcpf || scalar_op == kScalarRsqf)
        {
          r.rcpf_rsqf.push_back({my_ordinal, at, scalar_op});
        }
        const uint32_t vector_dest = d0 & 0x3F;
        const bool vector_rel = (d0 >> 6) & 1u;
        const bool exported = (d0 >> 15) & 1u;
        const uint32_t vector_mask = (d0 >> 16) & 0xF;
        const uint32_t src3_swizzle = d1 & 0xFF;
        const uint32_t src3_reg = d2 & 0xFF;
        const uint32_t vector_op = (d2 >> 24) & 0x1F;
        const bool src3_temp = (d2 >> 29) & 1u;
        if (exported || vector_mask == 0 || vector_rel || ScalarNoSource(scalar_op))
        {
          continue;
        }
        // Temporary register components the scalar operation reads.
        uint32_t reads_reg[2] = {~0u, ~0u}, reads_comp[2] = {0, 0};
        if (scalar_op >= kScalarConstFirst && scalar_op <= kScalarConstLast)
        {
          // Constant x temporary: the temporary is (opcode bit 0) | (src3_sel << 1) | (src3 swizzle bits 2:5), `b` = X.
          reads_reg[0] = (scalar_op & 1u) | (uint32_t(src3_temp) << 1) | (src3_swizzle & 0x3Cu);
          reads_comp[0] = SwizzledComponent(src3_swizzle, 0);
        }
        else
        {
          if (!src3_temp || (src3_reg & 0x40u))
          {
            continue; // a constant, or an aL-relative register (not resolved statically)
          }
          reads_reg[0] = src3_reg & 0x3Fu;
          reads_comp[0] = SwizzledComponent(src3_swizzle, 3);
          if (ScalarTwoComponent(scalar_op))
          {
            reads_reg[1] = reads_reg[0];
            reads_comp[1] = SwizzledComponent(src3_swizzle, VectorOperandCount(vector_op) == 3 ? 2 : 0);
          }
        }
        bool hit = false;
        for (uint32_t i = 0; i < 2; ++i)
        {
          hit |= reads_reg[i] == vector_dest && ((vector_mask >> reads_comp[i]) & 1u);
        }
        if (hit)
        {
          Hazard h;
          h.ordinal = my_ordinal;
          h.address = at;
          h.vector_dest = vector_dest;
          h.vector_mask = vector_mask;
          h.vector_opcode = vector_op;
          h.scalar_opcode = scalar_op;
          r.hazards.push_back(h);
        }
      }
    }
    r.scalar_ops = ordinal;
    r.ok = true;
    return r;
  }

  // Moves the scalar line(s) of every hazard above the vector line of the same instruction. False (with `why`) when the
  // text does not have the shape the fork emits (number of "ps = " lines, the vector line's register, a vector operation
  // that emits more than one line, maxas). The line separator of the text is kept.
  inline bool Patch(const std::string &hlsl, const ScanResult &scan, std::string &out, std::string &why)
  {
    using namespace scan_detail;
    if (!scan.ok)
    {
      why = "microcode not parsed";
      return false;
    }
    const bool crlf = hlsl.find("\r\n") != std::string::npos;
    std::vector<std::string> lines;
    for (size_t b = 0;;)
    {
      const size_t e = hlsl.find('\n', b);
      if (e == std::string::npos)
      {
        lines.push_back(hlsl.substr(b));
        break;
      }
      size_t end = e;
      if (crlf && end > b && hlsl[end - 1] == '\r')
      {
        --end;
      }
      lines.push_back(hlsl.substr(b, end - b));
      b = e + 1;
    }
    size_t start = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
    {
      if (StartsWith(Trim(lines[i]), "[shader("))
      {
        start = i;
        break;
      }
    }
    if (start == lines.size())
    {
      why = "no [shader(...)] entry point";
      return false;
    }
    std::vector<size_t> ps_lines;
    for (size_t i = start; i < lines.size(); ++i)
    {
      if (StartsWith(Trim(lines[i]), "ps = "))
      {
        ps_lines.push_back(i);
      }
    }
    if (ps_lines.size() != scan.scalar_ops)
    {
      why = std::to_string(ps_lines.size()) + " 'ps = ' lines for " + std::to_string(scan.scalar_ops) +
            " scalar operations";
      return false;
    }
    struct Move
    {
      size_t vector_line, last; // the vector line, and the last scalar line: [vector_line + 1, last] moves above it
    };
    std::vector<Move> moves;
    for (const Hazard &h : scan.hazards)
    {
      if (h.scalar_opcode == kScalarMaxAs || h.scalar_opcode == kScalarMaxAsf)
      {
        why = At(h.address) + "maxas reads its operand again for a0";
        return false;
      }
      if (!VectorOneLine(h.vector_opcode))
      {
        why = At(h.address) + "vector " + VectorName(h.vector_opcode) + " emits two lines";
        return false;
      }
      if (h.ordinal >= ps_lines.size())
      {
        why = At(h.address) + "scalar ordinal out of range";
        return false;
      }
      const size_t last = ps_lines[h.ordinal];
      size_t first = last;
      if (h.scalar_opcode >= kScalarSetpFirst && h.scalar_opcode <= kScalarSetpLast)
      {
        if (last == 0 || !StartsWith(Trim(lines[last - 1]), "p0 = "))
        {
          why = At(h.address) + "no 'p0 = ' line before the scalar line";
          return false;
        }
        first = last - 1;
      }
      if (first <= start)
      {
        why = At(h.address) + "no line before the scalar line";
        return false;
      }
      const std::string vector_line = Trim(lines[first - 1]);
      const std::string want = "r" + std::to_string(h.vector_dest) + ".";
      if (!StartsWith(vector_line, want) || vector_line.find(" = ") == std::string::npos || vector_line.back() != ';')
      {
        why = At(h.address) + "the line before the scalar line is '" + vector_line.substr(0, 80) + "', not the write of r" +
              std::to_string(h.vector_dest);
        return false;
      }
      moves.push_back({first - 1, last});
    }
    // Different instructions: disjoint ranges, each [vector_line, last] becomes the scalar line(s) then the vector line.
    for (const Move &m : moves)
    {
      const std::string vector_line = lines[m.vector_line];
      for (size_t i = m.vector_line; i < m.last; ++i)
      {
        lines[i] = lines[i + 1];
      }
      lines[m.last] = vector_line;
    }
    out.clear();
    out.reserve(hlsl.size() + 16);
    for (size_t i = 0; i < lines.size(); ++i)
    {
      out += lines[i];
      if (i + 1 < lines.size())
      {
        out += crlf ? "\r\n" : "\n";
      }
    }
    return true;
  }
} // namespace pdz::native::coissue
