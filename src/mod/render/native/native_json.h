// Native renderer (stage 1): a small JSON reader for the shader sidecars (<xxh3>.json next to <xxh3>.dxil)
// and the harvest manifest. No dependency: the SDK's nlohmann copy lives under thirdparty/inja, which is not on
// the game's include set, so a 200-line recursive-descent parser is used instead (PIPELINE_NOTES.md).
// Cold path only (shader load time). Numbers are doubles (every sidecar integer fits), hashes are strings.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace pdz::native::json
{
  struct Value
  {
    enum class Type : uint8_t
    {
      kNull,
      kBool,
      kNumber,
      kString,
      kArray,
      kObject
    };

    Type type = Type::kNull;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<Value> items;      // kArray: the elements; kObject: the member values
    std::vector<std::string> keys; // kObject: the member names (parallel to items)

    bool IsNull() const { return type == Type::kNull; }
    bool IsArray() const { return type == Type::kArray; }
    bool IsObject() const { return type == Type::kObject; }
    bool IsString() const { return type == Type::kString; }
    bool IsNumber() const { return type == Type::kNumber; }
    size_t size() const { return items.size(); }
    const Value &operator[](size_t i) const { return items[i]; }

    // Object member (nullptr when absent or not an object).
    const Value *Get(std::string_view key) const
    {
      if (type != Type::kObject)
      {
        return nullptr;
      }
      for (size_t i = 0; i < keys.size(); ++i)
      {
        if (keys[i] == key)
        {
          return &items[i];
        }
      }
      return nullptr;
    }

    int64_t AsInt(int64_t def = 0) const { return type == Type::kNumber ? int64_t(number) : def; }
    uint32_t AsU32(uint32_t def = 0) const
    {
      return type == Type::kNumber ? uint32_t(int64_t(number)) : def;
    }
    double AsDouble(double def = 0.0) const { return type == Type::kNumber ? number : def; }
    bool AsBool(bool def = false) const { return type == Type::kBool ? boolean : def; }
    std::string_view AsString() const { return type == Type::kString ? std::string_view(str) : std::string_view(); }

    // Convenience: member as int / bool / string with a default.
    int64_t GetInt(std::string_view key, int64_t def = 0) const
    {
      const Value *v = Get(key);
      return v ? v->AsInt(def) : def;
    }
    bool GetBool(std::string_view key, bool def = false) const
    {
      const Value *v = Get(key);
      return v ? v->AsBool(def) : def;
    }
    std::string_view GetString(std::string_view key) const
    {
      const Value *v = Get(key);
      return v ? v->AsString() : std::string_view();
    }
  };

  class Parser
  {
  public:
    Parser(std::string_view text, std::string *error) : text_(text), error_(error) {}

    bool ParseDocument(Value &out)
    {
      SkipWs();
      if (!ParseValue(out, 0))
      {
        return false;
      }
      SkipWs();
      if (pos_ != text_.size())
      {
        return Fail("trailing characters");
      }
      return true;
    }

  private:
    static constexpr int kMaxDepth = 64;

    bool Fail(const char *what)
    {
      if (error_ && error_->empty())
      {
        *error_ = std::string(what) + " at offset " + std::to_string(pos_);
      }
      return false;
    }

    void SkipWs()
    {
      while (pos_ < text_.size())
      {
        const char c = text_[pos_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
          ++pos_;
        }
        else
        {
          break;
        }
      }
    }

    bool Match(std::string_view literal)
    {
      if (text_.substr(pos_, literal.size()) == literal)
      {
        pos_ += literal.size();
        return true;
      }
      return false;
    }

    bool ParseValue(Value &out, int depth)
    {
      if (depth > kMaxDepth)
      {
        return Fail("nesting too deep");
      }
      if (pos_ >= text_.size())
      {
        return Fail("unexpected end");
      }
      const char c = text_[pos_];
      switch (c)
      {
      case '{':
        return ParseObject(out, depth);
      case '[':
        return ParseArray(out, depth);
      case '"':
        out.type = Value::Type::kString;
        return ParseString(out.str);
      case 't':
        if (Match("true"))
        {
          out.type = Value::Type::kBool;
          out.boolean = true;
          return true;
        }
        return Fail("bad literal");
      case 'f':
        if (Match("false"))
        {
          out.type = Value::Type::kBool;
          out.boolean = false;
          return true;
        }
        return Fail("bad literal");
      case 'n':
        if (Match("null"))
        {
          out.type = Value::Type::kNull;
          return true;
        }
        return Fail("bad literal");
      default:
        if (c == '-' || (c >= '0' && c <= '9'))
        {
          return ParseNumber(out);
        }
        return Fail("unexpected character");
      }
    }

    bool ParseNumber(Value &out)
    {
      const size_t start = pos_;
      while (pos_ < text_.size())
      {
        const char c = text_[pos_];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
        {
          ++pos_;
        }
        else
        {
          break;
        }
      }
      char buf[64];
      const size_t len = pos_ - start;
      if (len == 0 || len >= sizeof(buf))
      {
        return Fail("bad number");
      }
      text_.copy(buf, len, start);
      buf[len] = 0;
      char *end = nullptr;
      out.type = Value::Type::kNumber;
      out.number = std::strtod(buf, &end);
      if (end != buf + len)
      {
        return Fail("bad number");
      }
      return true;
    }

    static void AppendUtf8(std::string &s, uint32_t cp)
    {
      if (cp < 0x80)
      {
        s.push_back(char(cp));
      }
      else if (cp < 0x800)
      {
        s.push_back(char(0xC0 | (cp >> 6)));
        s.push_back(char(0x80 | (cp & 0x3F)));
      }
      else
      {
        s.push_back(char(0xE0 | (cp >> 12)));
        s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(char(0x80 | (cp & 0x3F)));
      }
    }

    bool ParseString(std::string &out)
    {
      // text_[pos_] == '"'
      ++pos_;
      out.clear();
      while (pos_ < text_.size())
      {
        const char c = text_[pos_++];
        if (c == '"')
        {
          return true;
        }
        if (c != '\\')
        {
          out.push_back(c);
          continue;
        }
        if (pos_ >= text_.size())
        {
          break;
        }
        const char e = text_[pos_++];
        switch (e)
        {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u':
        {
          if (pos_ + 4 > text_.size())
          {
            return Fail("bad \\u escape");
          }
          uint32_t cp = 0;
          for (int i = 0; i < 4; ++i)
          {
            const char h = text_[pos_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= uint32_t(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= uint32_t(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= uint32_t(h - 'A' + 10);
            else return Fail("bad \\u escape");
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          return Fail("bad escape");
        }
      }
      return Fail("unterminated string");
    }

    bool ParseArray(Value &out, int depth)
    {
      ++pos_; // '['
      out.type = Value::Type::kArray;
      SkipWs();
      if (pos_ < text_.size() && text_[pos_] == ']')
      {
        ++pos_;
        return true;
      }
      for (;;)
      {
        SkipWs();
        out.items.emplace_back();
        if (!ParseValue(out.items.back(), depth + 1))
        {
          return false;
        }
        SkipWs();
        if (pos_ >= text_.size())
        {
          return Fail("unterminated array");
        }
        const char c = text_[pos_++];
        if (c == ']')
        {
          return true;
        }
        if (c != ',')
        {
          return Fail("expected , or ]");
        }
      }
    }

    bool ParseObject(Value &out, int depth)
    {
      ++pos_; // '{'
      out.type = Value::Type::kObject;
      SkipWs();
      if (pos_ < text_.size() && text_[pos_] == '}')
      {
        ++pos_;
        return true;
      }
      for (;;)
      {
        SkipWs();
        if (pos_ >= text_.size() || text_[pos_] != '"')
        {
          return Fail("expected member name");
        }
        std::string key;
        if (!ParseString(key))
        {
          return false;
        }
        SkipWs();
        if (pos_ >= text_.size() || text_[pos_] != ':')
        {
          return Fail("expected :");
        }
        ++pos_;
        SkipWs();
        out.keys.push_back(std::move(key));
        out.items.emplace_back();
        if (!ParseValue(out.items.back(), depth + 1))
        {
          return false;
        }
        SkipWs();
        if (pos_ >= text_.size())
        {
          return Fail("unterminated object");
        }
        const char c = text_[pos_++];
        if (c == '}')
        {
          return true;
        }
        if (c != ',')
        {
          return Fail("expected , or }");
        }
      }
    }

    std::string_view text_;
    size_t pos_ = 0;
    std::string *error_;
  };

  inline bool Parse(std::string_view text, Value &out, std::string *error = nullptr)
  {
    if (error)
    {
      error->clear();
    }
    Parser p(text, error);
    return p.ParseDocument(out);
  }
} // namespace pdz::native::json
