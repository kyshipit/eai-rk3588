// from https://github.com/ml-inory/melotts.axera/blob/main/cpp/src/split_utils.hpp

#pragma once

#include <algorithm>
#include <cctype>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

// 判断是否是UTF-8字符的后续字节
inline bool is_utf8_continuation_byte(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// 计算UTF-8字符串的字符数（非字节数）
inline size_t utf8_strlen(const std::string& str) {
    size_t len = 0;
    for (size_t i = 0; i < str.size();) {
        unsigned char c = str[i];
        if ((c & 0x80) == 0) {
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i++;
        }
        len++;
    }
    return len;
}

// 合并短句的英文版本
inline std::vector<std::string> merge_short_sentences_en(const std::vector<std::string>& sens) {
    std::vector<std::string> sens_out;
    for (const auto& s : sens) {
        if (!sens_out.empty()) {
            std::istringstream iss(sens_out.back());
            int word_count = static_cast<int>(std::distance(
                std::istream_iterator<std::string>(iss), std::istream_iterator<std::string>()));
            if (word_count <= 2) {
                sens_out.back() += " " + s;
                continue;
            }
        }
        sens_out.push_back(s);
    }

    if (!sens_out.empty() && sens_out.size() > 1) {
        std::istringstream iss(sens_out.back());
        int word_count = static_cast<int>(std::distance(
            std::istream_iterator<std::string>(iss), std::istream_iterator<std::string>()));
        if (word_count <= 2) {
            sens_out[sens_out.size() - 2] += " " + sens_out.back();
            sens_out.pop_back();
        }
    }

    return sens_out;
}

// 合并短句的中文版本
inline std::vector<std::string> merge_short_sentences_zh(const std::vector<std::string>& sens) {
    std::vector<std::string> sens_out;
    for (const auto& s : sens) {
        if (!sens_out.empty() && utf8_strlen(sens_out.back()) <= 2) {
            sens_out.back() += " " + s;
        } else {
            sens_out.push_back(s);
        }
    }

    if (!sens_out.empty() && sens_out.size() > 1 && utf8_strlen(sens_out.back()) <= 2) {
        sens_out[sens_out.size() - 2] += " " + sens_out.back();
        sens_out.pop_back();
    }

    return sens_out;
}

// 替换字符串中的子串
inline std::string replace_all(const std::string& input, const std::string& from,
                               const std::string& to) {
    std::string result = input;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.length(), to);
        pos += to.length();
    }
    return result;
}

// 分割拉丁语系文本（英文、法文、西班牙文等）
inline std::vector<std::string> split_sentences_latin(const std::string& text, int min_len = 10) {
    (void)min_len;
    std::string processed = text;

    processed = replace_all(processed, "。", ".");
    processed = replace_all(processed, "！", ".");
    processed = replace_all(processed, "？", ".");
    processed = replace_all(processed, "；", ".");
    processed = replace_all(processed, "，", ",");
    processed = replace_all(processed, "“", "\"");
    processed = replace_all(processed, "”", "\"");
    processed = replace_all(processed, "‘", "'");
    processed = replace_all(processed, "’", "'");

    const std::string chars_to_remove = "<>[]\"«»";
    for (char c : chars_to_remove) {
        processed.erase(std::remove(processed.begin(), processed.end(), c), processed.end());
    }

    std::vector<std::string> sentences;
    size_t start = 0;
    size_t end = processed.find('.');
    while (end != std::string::npos) {
        std::string sentence = processed.substr(start, end - start);
        sentence.erase(sentence.begin(),
                       std::find_if(sentence.begin(), sentence.end(),
                                    [](int ch) { return !std::isspace(ch); }));
        sentence.erase(std::find_if(sentence.rbegin(), sentence.rend(),
                                    [](int ch) { return !std::isspace(ch); })
                           .base(),
                       sentence.end());
        if (!sentence.empty()) {
            sentences.push_back(sentence);
        }
        start = end + 1;
        end = processed.find('.', start);
    }

    if (start < processed.size()) {
        std::string sentence = processed.substr(start);
        sentence.erase(sentence.begin(),
                       std::find_if(sentence.begin(), sentence.end(),
                                    [](int ch) { return !std::isspace(ch); }));
        sentence.erase(std::find_if(sentence.rbegin(), sentence.rend(),
                                    [](int ch) { return !std::isspace(ch); })
                           .base(),
                       sentence.end());
        if (!sentence.empty()) {
            sentences.push_back(sentence);
        }
    }

    return merge_short_sentences_en(sentences);
}

// 分割中文文本
inline std::vector<std::string> split_sentences_zh(const std::string& text, int min_len = 10) {
    std::string processed = text;

    processed = replace_all(processed, "。", ".");
    processed = replace_all(processed, "！", ".");
    processed = replace_all(processed, "？", ".");
    processed = replace_all(processed, "；", ".");
    processed = replace_all(processed, "，", ",");
    processed = replace_all(processed, "\n", " ");
    processed = replace_all(processed, "\t", " ");
    processed = replace_all(processed, "  ", " ");

    const std::string punctuation = ".,!?;";
    for (char c : punctuation) {
        const std::string from(1, c);
        const std::string to = from + " $#!";
        processed = replace_all(processed, from, to);
    }

    std::vector<std::string> sentences;
    size_t start = 0;
    size_t end = processed.find("$#!");
    while (end != std::string::npos) {
        std::string sentence = processed.substr(start, end - start);
        sentence.erase(sentence.begin(),
                       std::find_if(sentence.begin(), sentence.end(),
                                    [](int ch) { return !std::isspace(ch); }));
        sentence.erase(std::find_if(sentence.rbegin(), sentence.rend(),
                                    [](int ch) { return !std::isspace(ch); })
                           .base(),
                       sentence.end());
        if (!sentence.empty()) {
            sentences.push_back(sentence);
        }
        start = end + 3;
        end = processed.find("$#!", start);
    }

    if (start < processed.size()) {
        std::string sentence = processed.substr(start);
        sentence.erase(sentence.begin(),
                       std::find_if(sentence.begin(), sentence.end(),
                                    [](int ch) { return !std::isspace(ch); }));
        sentence.erase(std::find_if(sentence.rbegin(), sentence.rend(),
                                    [](int ch) { return !std::isspace(ch); })
                           .base(),
                       sentence.end());
        if (!sentence.empty()) {
            sentences.push_back(sentence);
        }
    }

    std::vector<std::string> new_sentences;
    std::vector<std::string> new_sent;
    int count_len = 0;
    for (size_t i = 0; i < sentences.size(); ++i) {
        new_sent.push_back(sentences[i]);
        count_len += static_cast<int>(utf8_strlen(sentences[i]));
        if (count_len > min_len || i == sentences.size() - 1) {
            count_len = 0;
            std::ostringstream oss;
            for (size_t j = 0; j < new_sent.size(); ++j) {
                if (j != 0) {
                    oss << " ";
                }
                oss << new_sent[j];
            }
            new_sentences.push_back(oss.str());
            new_sent.clear();
        }
    }

    return merge_short_sentences_zh(new_sentences);
}

// 主分割函数
inline std::vector<std::string> split_sentence(const std::string& text, int min_len = 10,
                                               const std::string& language_str = "EN") {
    if (language_str == "EN" || language_str == "FR" || language_str == "ES" ||
        language_str == "SP") {
        return split_sentences_latin(text, min_len);
    }
    return split_sentences_zh(text, min_len);
}
