/*
 * app/config_parser.h（header-only）配置解析器
 */

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

enum class ConfigType {
    Null,
    String,
    Number,
    Array,
    Object
};

// 轻量 YAML-like 解析器：支持对象、标量、数组和 . 路径读取。
class ConfigParser {
private:
    ConfigType type_ = ConfigType::Null;
    std::string str_val_;
    double num_val_ = 0.0;
    std::vector<ConfigParser> arr_val_;
    std::map<std::string, ConfigParser> children_;

    // 去除字符串首尾空白字符（空串安全）。
    static std::string trim(const std::string& s) {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
            ++start;
        }
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
            --end;
        }
        return s.substr(start, end - start);
    }

    // 剥离 YAML 行内注释：仅在引号外遇到 '#' 才截断。
    static std::string strip_inline_comment(const std::string& s) {
        bool in_single = false;
        bool in_double = false;
        bool escaped = false;
        for (size_t i = 0; i < s.size(); ++i) {
            const char ch = s[i];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\' && in_double) {
                escaped = true;
                continue;
            }
            if (ch == '\'' && !in_double) {
                in_single = !in_single;
                continue;
            }
            if (ch == '"' && !in_single) {
                in_double = !in_double;
                continue;
            }
            if (ch == '#' && !in_single && !in_double) {
                return trim(s.substr(0, i));
            }
        }
        return trim(s);
    }

    // 仅当首尾引号匹配时去引号。
    static std::string unquote_if_needed(const std::string& s) {
        if (s.size() < 2) {
            return s;
        }
        const char first = s.front();
        const char last = s.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return s.substr(1, s.size() - 2);
        }
        return s;
    }

    // 按字符分割，忽略空元素。
    static std::vector<std::string> split_simple(const std::string& s, char delim) {
        std::vector<std::string> res;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, delim)) {
            item = trim(item);
            if (!item.empty()) {
                res.push_back(item);
            }
        }
        return res;
    }

    // 数组元素分割：引号内的 ',' 不作为分隔符。
    static std::vector<std::string> split_array_elements(const std::string& s) {
        std::vector<std::string> res;
        std::string cur;
        bool in_single = false;
        bool in_double = false;
        bool escaped = false;
        for (size_t i = 0; i < s.size(); ++i) {
            const char ch = s[i];
            if (escaped) {
                cur.push_back(ch);
                escaped = false;
                continue;
            }
            if (ch == '\\' && in_double) {
                cur.push_back(ch);
                escaped = true;
                continue;
            }
            if (ch == '\'' && !in_double) {
                in_single = !in_single;
                cur.push_back(ch);
                continue;
            }
            if (ch == '"' && !in_single) {
                in_double = !in_double;
                cur.push_back(ch);
                continue;
            }
            if (ch == ',' && !in_single && !in_double) {
                std::string elem = trim(cur);
                if (!elem.empty()) {
                    res.push_back(elem);
                }
                cur.clear();
                continue;
            }
            cur.push_back(ch);
        }
        std::string elem = trim(cur);
        if (!elem.empty()) {
            res.push_back(elem);
        }
        return res;
    }

    // 判定是否可完整解析为数字。
    static bool is_number(const std::string& s) {
        if (s.empty()) {
            return false;
        }
        char* end = nullptr;
        std::strtod(s.c_str(), &end);
        return end == s.c_str() + s.size();
    }

    // 解析值（字符串/数字/数组）。
    void parse_value(const std::string& val_str) {
        std::string val = trim(val_str);
        if (val.empty()) {
            type_ = ConfigType::Null;
            str_val_.clear();
            num_val_ = 0.0;
            arr_val_.clear();
            children_.clear();
            return;
        }

        if (val.size() >= 2 && val.front() == '[' && val.back() == ']') {
            type_ = ConfigType::Array;
            arr_val_.clear();
            std::string content = val.substr(1, val.size() - 2);
            auto elements = split_array_elements(content);
            for (const auto& elem : elements) {
                ConfigParser node;
                node.parse_value(elem);
                arr_val_.push_back(node);
            }
            return;
        }

        const std::string unquoted = unquote_if_needed(val);
        if (is_number(unquoted)) {
            type_ = ConfigType::Number;
            num_val_ = std::stod(unquoted);
            return;
        }

        type_ = ConfigType::String;
        str_val_ = unquoted;
    }

    // 按 . 分割键（如 model.type -> [model, type]）。
    static std::vector<std::string> split_dot_key(const std::string& key) {
        return split_simple(key, '.');
    }

    // 根据键路径查找节点。
    ConfigParser* find_node(const std::string& key_path) {
        auto keys = split_dot_key(key_path);
        ConfigParser* node = this;
        for (const auto& k : keys) {
            auto it = node->children_.find(k);
            if (it == node->children_.end()) {
                return nullptr;
            }
            node = &(it->second);
        }
        return node;
    }

public:
    // 从文件加载配置。
    bool LoadFromFile(const std::string& path) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "[Error] 无法打开文件: " << path << std::endl;
            return false;
        }

        // 重置根节点，避免重复加载残留旧值。
        type_ = ConfigType::Object;
        str_val_.clear();
        num_val_ = 0.0;
        arr_val_.clear();
        children_.clear();

        std::vector<ConfigParser*> stack;
        std::vector<size_t> stack_indent;
        stack.push_back(this);
        stack_indent.push_back(0);
        std::string line;

        while (std::getline(file, line)) {
            std::string trim_line = trim(line);
            if (trim_line.empty() || trim_line[0] == '#') {
                continue;
            }

            size_t indent = 0;
            while (indent < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[indent])) != 0) {
                ++indent;
            }
            std::string content = line.substr(indent);
            content = strip_inline_comment(content);
            if (content.empty()) {
                continue;
            }

            const size_t colon = content.find(':');
            if (colon == std::string::npos) {
                continue;
            }

            const std::string key = trim(content.substr(0, colon));
            std::string val = trim(content.substr(colon + 1));
            if (key.empty()) {
                continue;
            }

            while (stack.size() > 1 && indent <= stack_indent.back()) {
                stack.pop_back();
                stack_indent.pop_back();
            }

            ConfigParser& child = stack.back()->children_[key];
            if (!val.empty()) {
                child.parse_value(val);
            } else {
                child.type_ = ConfigType::Object;
                stack.push_back(&child);
                stack_indent.push_back(indent);
            }
        }

        file.close();
        return true;
    }

    // 获取字符串（支持 . 嵌套，带默认值）。
    std::string GetString(const std::string& key, const std::string& default_val = "") {
        ConfigParser* node = find_node(key);
        if (!node) {
            return default_val;
        }
        if (node->type_ == ConfigType::String) {
            return node->str_val_;
        }
        if (node->type_ == ConfigType::Number) {
            std::ostringstream oss;
            oss << node->num_val_;
            return oss.str();
        }
        return default_val;
    }

    // 获取整数（支持 . 嵌套，带默认值）。
    int GetInt(const std::string& key, int default_val = 0) {
        ConfigParser* node = find_node(key);
        if (!node) {
            return default_val;
        }
        if (node->type_ == ConfigType::Number) {
            return static_cast<int>(node->num_val_);
        }
        if (node->type_ == ConfigType::String && is_number(node->str_val_)) {
            return static_cast<int>(std::stod(node->str_val_));
        }
        return default_val;
    }

    // 获取布尔：支持 number/string。
    bool GetBool(const std::string& key, bool default_val = false) {
        ConfigParser* node = find_node(key);
        if (!node) {
            return default_val;
        }
        if (node->type_ == ConfigType::Number) {
            return node->num_val_ != 0.0;
        }
        if (node->type_ == ConfigType::String) {
            std::string s = node->str_val_;
            std::transform(
                s.begin(),
                s.end(),
                s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (s == "true" || s == "yes" || s == "on" || s == "1") {
                return true;
            }
            if (s == "false" || s == "no" || s == "off" || s == "0") {
                return false;
            }
        }
        return default_val;
    }

    // 获取数组（支持 . 嵌套）。
    std::vector<ConfigParser> GetArray(const std::string& key) {
        ConfigParser* node = find_node(key);
        if (!node || node->type_ != ConfigType::Array) {
            return {};
        }
        return node->arr_val_;
    }

    // 获取整数数组。
    std::vector<int> GetIntArray(const std::string& key) {
        std::vector<int> res;
        auto arr = GetArray(key);
        for (const auto& n : arr) {
            res.push_back(n.as_int());
        }
        return res;
    }

    // 将当前节点按数字读取为 int（主要供数组元素转换）。
    int as_int() const {
        return static_cast<int>(num_val_);
    }
};
