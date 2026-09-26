// xrkit::core —— 构建版本与构建史的实现（口径见 build_version.h）。
#include "xrkit/core/build_version.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "xrkit/core/path.h"

namespace xrkit::core {
namespace {

namespace stdfs = std::filesystem;

// ── 极简 JSON（只覆盖 version.json 的形状：对象 / 数组 / 字符串 / 整数）──────
// 不引第三方：这个文件是我们自己写的，schema 也由我们定；用一个够用的子集解析器
// 比拉一个大依赖更可控，也满足"core 不许 include Qt/重依赖"。
struct JVal {
    enum class Kind { Null, Int, Str, Arr, Obj } kind = Kind::Null;
    long long num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;  // 保序

    const JVal* find(const std::string& key) const {
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

class JParser {
public:
    explicit JParser(const std::string& text) : s_(text) {}

    bool parse(JVal& out, std::string& err) {
        skip_ws();
        if (!value(out, err)) return false;
        skip_ws();
        return true;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void skip_ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\r' ||
                                  s_[i_] == '\n')) {
            ++i_;
        }
    }
    bool lit(const char* word) {
        const size_t n = std::char_traits<char>::length(word);
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }
    bool str(std::string& out, std::string& err) {
        if (i_ >= s_.size() || s_[i_] != '"') {
            err = "JSON：期望字符串";
            return false;
        }
        ++i_;
        out.clear();
        while (i_ < s_.size() && s_[i_] != '"') {
            if (s_[i_] == '\\' && i_ + 1 < s_.size()) {
                ++i_;
                const char esc = s_[i_++];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'u': {
                        // 只处理 BMP 的 \uXXXX ⇒ UTF-8（我们写出来的都是 ASCII，够用）
                        if (i_ + 4 > s_.size()) {
                            err = "JSON：\\u 转义不完整";
                            return false;
                        }
                        unsigned cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char c = s_[i_ + static_cast<size_t>(k)];
                            cp <<= 4;
                            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
                            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
                            else { err = "JSON：\\u 里有非法十六进制"; return false; }
                        }
                        i_ += 4;
                        if (cp < 0x80) {
                            out.push_back(static_cast<char>(cp));
                        } else if (cp < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(s_[i_++]);
            }
        }
        if (i_ >= s_.size()) {
            err = "JSON：字符串没有收尾引号";
            return false;
        }
        ++i_;  // 收尾引号
        return true;
    }
    bool value(JVal& out, std::string& err) {
        skip_ws();
        if (i_ >= s_.size()) {
            err = "JSON：意外结束";
            return false;
        }
        const char c = s_[i_];
        if (c == '{') {
            ++i_;
            out.kind = JVal::Kind::Obj;
            skip_ws();
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
            while (true) {
                skip_ws();
                std::string key;
                if (!str(key, err)) return false;
                skip_ws();
                if (i_ >= s_.size() || s_[i_] != ':') {
                    err = "JSON：对象里缺冒号";
                    return false;
                }
                ++i_;
                JVal v;
                if (!value(v, err)) return false;
                out.obj.emplace_back(std::move(key), std::move(v));
                skip_ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                err = "JSON：对象里缺 , 或 }";
                return false;
            }
        }
        if (c == '[') {
            ++i_;
            out.kind = JVal::Kind::Arr;
            skip_ws();
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
            while (true) {
                JVal v;
                if (!value(v, err)) return false;
                out.arr.push_back(std::move(v));
                skip_ws();
                if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                err = "JSON：数组里缺 , 或 ]";
                return false;
            }
        }
        if (c == '"') {
            out.kind = JVal::Kind::Str;
            return str(out.str, err);
        }
        if (lit("true")) { out.kind = JVal::Kind::Int; out.num = 1; return true; }
        if (lit("false")) { out.kind = JVal::Kind::Int; out.num = 0; return true; }
        if (lit("null")) { out.kind = JVal::Kind::Null; return true; }
        // 数字（只支持整数：我们的 schema 里没有小数）
        size_t j = i_;
        if (j < s_.size() && (s_[j] == '-' || s_[j] == '+')) ++j;
        const size_t digits_begin = j;
        while (j < s_.size() && std::isdigit(static_cast<unsigned char>(s_[j]))) ++j;
        if (j == digits_begin) {
            err = "JSON：无法识别的记号";
            return false;
        }
        out.kind = JVal::Kind::Int;
        out.num = std::stoll(s_.substr(i_, j - i_));
        i_ = j;
        return true;
    }
};

std::string esc(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

bool read_text(const std::string& path, std::string& out, std::string& error) {
    std::ifstream f(path_from_utf8(path), std::ios::binary);
    if (!f) {
        error = "打不开 " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool write_text(const std::string& path, const std::string& text, std::string& error) {
    const stdfs::path p = path_from_utf8(path);
    std::error_code ec;
    if (p.has_parent_path()) stdfs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "写不了 " + path;
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        error = "写入失败：" + path;
        return false;
    }
    return true;
}

}  // namespace

std::vector<std::string> VersionStore::versions() const {
    std::vector<std::string> out;
    out.reserve(tree.size());
    for (const auto& kv : tree) out.push_back(kv.first);
    return out;
}

std::vector<std::string> VersionStore::prereleases(const std::string& version) const {
    std::vector<std::string> out{"ALPHA", "BETA", "RC"};  // 标准三档先给出来
    const auto it = tree.find(version);
    if (it != tree.end()) {
        for (const auto& kv : it->second) {
            if (std::find(out.begin(), out.end(), kv.first) == out.end()) out.push_back(kv.first);
        }
    }
    return out;
}

int VersionStore::next_index(const std::string& version, const std::string& prerelease) const {
    const auto v = tree.find(version);
    if (v == tree.end()) return 1;
    const auto m = v->second.find(prerelease);
    if (m == v->second.end() || m->second.empty()) return 1;
    int max_index = 0;
    for (const auto& kv : m->second) max_index = std::max(max_index, kv.first);
    return max_index + 1;
}

void VersionStore::append(const std::string& version, const std::string& prerelease, int index,
                          const BuildRecord& record) {
    tree[version][prerelease][index] = record;
}

std::string VersionStore::full_version() const {
    std::string out = current_version;
    if (!current_prerelease.empty()) {
        out += " " + current_prerelease;
        if (current_index > 0) out += "." + std::to_string(current_index);
    }
    return out;
}

std::string version_backup_path(const std::string& path) { return path + ".bak"; }

bool load_version_store(const std::string& path, VersionStore& out, std::string& error) {
    std::string text;
    if (!read_text(path, text, error)) return false;
    JVal root;
    JParser parser(text);
    if (!parser.parse(root, error)) return false;
    if (root.kind != JVal::Kind::Obj) {
        error = "version.json 顶层必须是对象";
        return false;
    }
    out = VersionStore{};
    if (const JVal* cur = root.find("current"); cur != nullptr && cur->kind == JVal::Kind::Obj) {
        if (const JVal* v = cur->find("version"); v != nullptr) out.current_version = v->str;
        if (const JVal* p = cur->find("prerelease"); p != nullptr) out.current_prerelease = p->str;
        if (const JVal* i = cur->find("index"); i != nullptr && i->kind == JVal::Kind::Int) {
            out.current_index = static_cast<int>(i->num);
        }
    }
    const JVal* tree = root.find("tree");
    if (tree != nullptr && tree->kind == JVal::Kind::Obj) {
        for (const auto& vkv : tree->obj) {
            if (vkv.second.kind != JVal::Kind::Obj) continue;
            for (const auto& mkv : vkv.second.obj) {
                if (mkv.second.kind != JVal::Kind::Obj) continue;
                for (const auto& ikv : mkv.second.obj) {
                    int index = 0;
                    try {
                        index = std::stoi(ikv.first);
                    } catch (...) {
                        continue;  // 非数字键⇒跳过（不是我们的 schema）
                    }
                    BuildRecord rec;
                    if (ikv.second.kind == JVal::Kind::Obj) {
                        if (const JVal* b = ikv.second.find("built"); b != nullptr) rec.built = b->str;
                        if (const JVal* c = ikv.second.find("commit"); c != nullptr) {
                            rec.commit = c->str;
                        }
                        if (const JVal* a = ikv.second.find("artifacts");
                            a != nullptr && a->kind == JVal::Kind::Arr) {
                            for (const JVal& item : a->arr) rec.artifacts.push_back(item.str);
                        }
                    }
                    out.tree[vkv.first][mkv.first][index] = rec;
                }
            }
        }
    }
    return true;
}

bool save_version_store(const std::string& path, const VersionStore& store, std::string& error) {
    std::ostringstream o;
    o << "{\n  \"current\": { \"version\": \"" << esc(store.current_version)
      << "\", \"prerelease\": \"" << esc(store.current_prerelease)
      << "\", \"index\": " << store.current_index << " },\n  \"tree\": {";
    bool first_v = true;
    for (const auto& vkv : store.tree) {
        o << (first_v ? "\n" : ",\n") << "    \"" << esc(vkv.first) << "\": {";
        first_v = false;
        bool first_m = true;
        for (const auto& mkv : vkv.second) {
            o << (first_m ? "\n" : ",\n") << "      \"" << esc(mkv.first) << "\": {";
            first_m = false;
            bool first_i = true;
            for (const auto& ikv : mkv.second) {
                o << (first_i ? "\n" : ",\n") << "        \"" << ikv.first << "\": { \"built\": \""
                  << esc(ikv.second.built) << "\", \"commit\": \"" << esc(ikv.second.commit)
                  << "\", \"artifacts\": [";
                for (size_t k = 0; k < ikv.second.artifacts.size(); ++k) {
                    o << (k == 0 ? "" : ", ") << "\"" << esc(ikv.second.artifacts[k]) << "\"";
                }
                o << "] }";
                first_i = false;
            }
            o << "\n      }";
        }
        o << "\n    }";
    }
    o << (store.tree.empty() ? "}\n}\n" : "\n  }\n}\n");
    return write_text(path, o.str(), error);
}

bool backup_version_store(const std::string& path, const std::string& backup_path,
                          std::string& error) {
    if (!stdfs::exists(path_from_utf8(path))) {
        error = "备份失败：源文件不存在 " + path;
        return false;
    }
    std::string text;
    if (!read_text(path, text, error)) return false;
    return write_text(backup_path, text, error);
}

bool restore_version_store(const std::string& backup_path, const std::string& path,
                           std::string& error) {
    std::string text;
    if (!read_text(backup_path, text, error)) return false;
    return write_text(path, text, error);
}

}  // namespace xrkit::core
