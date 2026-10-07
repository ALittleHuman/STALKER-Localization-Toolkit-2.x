// xrkit::core —— 构建版本与构建史的实现（口径见 build_version.h）。
#include "xrkit/core/build_version.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "xrkit/core/atomic_write.h"
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
        // ★★ 2026-10-06（审计处理批·2-5 / §5.9 / G3）：修前这里裸 `std::stoll`。
        //   `version.json` 里一个超出 long long 的整数（`99999999999999999999`）会抛
        //   `std::out_of_range` —— 而这条链的调用方**都没有 try**（`load_version_store` →
        //   CLI `run_cli` 无顶层 catch → `main`）⇒ 异常穿透 ⇒ `std::terminate`
        //   （用户看到的是"程序直接没了"；同一文件 `:329` 的**键**解析早就 catch(...) 了）。
        //   口径：解析不了就**当解析失败**（走既有的"解析失败一个字节都不写"保护），不抛。
        try {
            out.num = std::stoll(s_.substr(i_, j - i_));
        } catch (const std::exception&) {
            err = "JSON：整数超出可表示范围";
            return false;
        }
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
    // ★★ 2026-10-06（审计处理批·2-5 后半 / §5.9）：修前这里是 `ofstream(trunc)` ——
    //   **非原子**写这份"用户构建史"（与全仓原子写纪律不一致）：写到一半断电/被杀就留下
    //   半截 JSON，而它是下一次构建要读回的那份文件。改用仓库现成的 `core::write_file_atomic`
    //   （`theme.cpp` 的 `user.ltx` 写侧同款）。
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    return core::write_file_atomic(path, bytes, /*atomic=*/true, error, core::WriteVerify::Streamed);
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
    // ★ 2026-09-27 实测踩到的坑：PowerShell `Set-Content -Encoding UTF8` / 记事本写出来的
    //   JSON **带 UTF-8 BOM**，解析器在位置 0 就撞上 0xEF ⇒ 报"无法识别的记号"，
    //   而调用方会把它当成"文件不存在"⇒ **把用户的历史构建记录覆盖掉**。
    //   这里先把 BOM 吃掉（语义上它就是"文件是 UTF-8"的标记，不是内容）。
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
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

// ── ★★ 2026-10-07（H1-2）：版本号派生写回（见头文件里的口径）──────────────────

namespace {

// 这两行是**唯一的**写回点（形态变了必须在这里改，且 `--check` 会红）。
constexpr const char* kPrereleaseLine = "inline constexpr const char* kPrerelease = ";
constexpr const char* kBuildCountLine = "inline constexpr int kBuildCount = ";

bool replace_value_line(std::string& text, const std::string& prefix, const std::string& value,
                        std::string& error) {
    const size_t at = text.find(prefix);
    if (at == std::string::npos) {
        error = std::string("version.h 里找不到这一行：") + prefix;
        return false;
    }
    const size_t end = text.find(';', at);
    if (end == std::string::npos) {
        error = std::string("version.h 里那一行没有分号：") + prefix;
        return false;
    }
    text.replace(at, end - at, prefix + value);
    return true;
}

}  // namespace

bool load_version_header(const std::string& path, std::string& prerelease, int& build_count,
                         std::string& error) {
    prerelease.clear();
    build_count = -1;
    std::string text;
    if (!read_text(path, text, error)) return false;

    const size_t p = text.find(kPrereleaseLine);
    if (p == std::string::npos) {
        error = "version.h 里找不到 kPrerelease 赋值行";
        return false;
    }
    const size_t pq = text.find('"', p);
    const size_t pq2 = (pq == std::string::npos) ? std::string::npos : text.find('"', pq + 1);
    if (pq == std::string::npos || pq2 == std::string::npos) {
        error = "version.h 里 kPrerelease 的值不是引号字符串";
        return false;
    }
    prerelease = text.substr(pq + 1, pq2 - pq - 1);

    const size_t b = text.find(kBuildCountLine);
    if (b == std::string::npos) {
        error = "version.h 里找不到 kBuildCount 赋值行";
        return false;
    }
    const size_t bs = text.find(';', b);
    if (bs == std::string::npos) {
        error = "version.h 里 kBuildCount 那一行没有分号";
        return false;
    }
    const std::string digits =
        text.substr(b + std::strlen(kBuildCountLine), bs - (b + std::strlen(kBuildCountLine)));
    try {
        size_t used = 0;
        build_count = std::stoi(digits, &used);
        if (used != digits.size()) {
            error = "version.h 里 kBuildCount 不是纯整数：" + digits;
            return false;
        }
    } catch (const std::exception&) {
        // ★ 审计 2-5 同族：解析失败不许抛穿（旧版 `stoll` 会把 CLI 整个 terminate）
        error = "version.h 里 kBuildCount 解析失败（不是整数）：" + digits;
        return false;
    }
    return true;
}

bool save_version_header(const std::string& path, const std::string& prerelease, int build_count,
                         std::string& error) {
    error.clear();
    // ① 先把"要写的东西"算出来（值必须合法：序号 ≥ 0；标记串不许带引号/分号）：
    if (build_count < 0) {
        error = "序号不能为负：" + std::to_string(build_count);
        return false;
    }
    for (char c : prerelease) {
        if (c == '"' || c == ';' || c == '\n' || c == '\r') {
            error = "标记串里有非法字符（引号/分号/换行）：" + prerelease;
            return false;
        }
    }
    // ② 读原文（保留其余字节原样）→ 只改两行：
    std::string text;
    if (!read_text(path, text, error)) return false;
    const std::string before = text;
    if (!replace_value_line(text, kPrereleaseLine, "\"" + prerelease + "\"", error)) return false;
    if (!replace_value_line(text, kBuildCountLine, std::to_string(build_count), error)) return false;
    if (text == before) {
        error = "写回没有改变任何字节（形态对不上？）—— " + path;
        return false;
    }
    // ③ **原子写**（同目录 .part → rename；半写的 version.h 会编出坏版本串）：
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    if (!write_file_atomic(path, bytes, /*atomic=*/true, error)) return false;
    // ④ 写后**读回校验**（不是"我以为我写了"）：
    std::string got_prerelease;
    int got_count = -1;
    std::string check_error;
    if (!load_version_header(path, got_prerelease, got_count, check_error)) {
        error = "写回后读不回来：" + check_error;
        return false;
    }
    if (got_prerelease != prerelease || got_count != build_count) {
        error = "写回后读回的值不一致（期望 " + prerelease + "/" + std::to_string(build_count) +
                "，实测 " + got_prerelease + "/" + std::to_string(got_count) + "）";
        return false;
    }
    return true;
}

bool last_release(const VersionStore& store, std::string& version, std::string& prerelease,
                  int& index, std::string& error) {
    version.clear();
    prerelease.clear();
    index = 0;
    error.clear();
    // ★ 遍历 tree 找**最大序号**（不是 `current_index`：那是"下一个号"，
    //   门禁的构建 GUI 一动就会把它推一格 —— 实测 59 → 60）。
    for (const auto& per_version : store.tree) {
        for (const auto& per_marker : per_version.second) {
            for (const auto& per_index : per_marker.second) {
                if (per_index.first > index) {
                    index = per_index.first;
                    version = per_version.first;
                    prerelease = per_marker.first;
                }
            }
        }
    }
    if (index <= 0) {
        error = "version.json 的 tree 里还没有任何成功发包的记录";
        return false;
    }
    return true;
}

std::string release_version_string(const std::string& version, const std::string& prerelease,
                                  int index) {
    if (prerelease.empty()) return version;
    if (index <= 0) return version + " " + prerelease;
    return version + " " + prerelease + "." + std::to_string(index);
}

}  // namespace xrkit::core
