#include "Composition/SmartPasteManager.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <cwctype>
#include <algorithm>

using namespace aiie;

// Original implementation
bool IsSQLOriginal(std::wstring_view trimmed) {
    auto startsWithCi = [](std::wstring_view s, std::wstring_view prefix) {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::towupper(s[i]) != std::towupper(prefix[i])) return false;
        }
        return true;
    };
    if (startsWithCi(trimmed, L"SELECT ") || startsWithCi(trimmed, L"INSERT INTO ") ||
        startsWithCi(trimmed, L"CREATE TABLE ") || startsWithCi(trimmed, L"UPDATE ")) {
        std::wstring upper;
        upper.reserve(trimmed.size());
        for (wchar_t c : trimmed) upper.push_back(static_cast<wchar_t>(std::towupper(c)));
        if (upper.find(L" FROM ") != std::wstring::npos || upper.find(L" WHERE ") != std::wstring::npos ||
            upper.find(L" VALUES ") != std::wstring::npos || upper.find(L" SET ") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

// Strategy 1: Fast ASCII case-insensitive search
inline wchar_t FastToUpper(wchar_t c) {
    if (c >= L'a' && c <= L'z') return c - (L'a' - L'A');
    if (c < 128) return c;
    return static_cast<wchar_t>(std::towupper(c));
}

bool IsSQLFastAscii(std::wstring_view trimmed) {
    auto startsWithCi = [](std::wstring_view s, std::wstring_view prefix) {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (FastToUpper(s[i]) != prefix[i]) return false; // prefix is already uppercase ASCII!
        }
        return true;
    };

    if (startsWithCi(trimmed, L"SELECT ") || startsWithCi(trimmed, L"INSERT INTO ") ||
        startsWithCi(trimmed, L"CREATE TABLE ") || startsWithCi(trimmed, L"UPDATE ")) {

        auto containsCi = [](std::wstring_view haystack, std::wstring_view needle) {
            if (needle.empty()) return true;
            if (haystack.size() < needle.size()) return false;
            const wchar_t first = needle[0]; // Assuming needle is uppercase ASCII
            const size_t maxPos = haystack.size() - needle.size();
            for (size_t i = 0; i <= maxPos; ++i) {
                if (FastToUpper(haystack[i]) == first) {
                    bool match = true;
                    for (size_t j = 1; j < needle.size(); ++j) {
                        if (FastToUpper(haystack[i + j]) != needle[j]) {
                            match = false;
                            break;
                        }
                    }
                    if (match) return true;
                }
            }
            return false;
        };

        if (containsCi(trimmed, L" FROM ") || containsCi(trimmed, L" WHERE ") ||
            containsCi(trimmed, L" VALUES ") || containsCi(trimmed, L" SET ")) {
            return true;
        }
    }
    return false;
}

// Strategy 2: std::wstring allocation replaced with stack buffer (std::array / thread_local / vector or inline upper view)
bool IsSQLUpperBuffer(std::wstring_view trimmed) {
    auto startsWithCi = [](std::wstring_view s, std::wstring_view prefix) {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (FastToUpper(s[i]) != prefix[i]) return false;
        }
        return true;
    };

    if (startsWithCi(trimmed, L"SELECT ") || startsWithCi(trimmed, L"INSERT INTO ") ||
        startsWithCi(trimmed, L"CREATE TABLE ") || startsWithCi(trimmed, L"UPDATE ")) {

        std::wstring upper;
        upper.reserve(trimmed.size());
        for (wchar_t c : trimmed) upper.push_back(FastToUpper(c));

        if (upper.find(L" FROM ") != std::wstring::npos || upper.find(L" WHERE ") != std::wstring::npos ||
            upper.find(L" VALUES ") != std::wstring::npos || upper.find(L" SET ") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

// Strategy 3: std::search with FastToUpper
bool IsSQLStdSearch(std::wstring_view trimmed) {
    auto startsWithCi = [](std::wstring_view s, std::wstring_view prefix) {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (FastToUpper(s[i]) != prefix[i]) return false;
        }
        return true;
    };

    if (startsWithCi(trimmed, L"SELECT ") || startsWithCi(trimmed, L"INSERT INTO ") ||
        startsWithCi(trimmed, L"CREATE TABLE ") || startsWithCi(trimmed, L"UPDATE ")) {

        auto containsCi = [](std::wstring_view haystack, std::wstring_view needle) {
            return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                               [](wchar_t c1, wchar_t c2) {
                                   return FastToUpper(c1) == c2;
                               }) != haystack.end();
        };

        if (containsCi(trimmed, L" FROM ") || containsCi(trimmed, L" WHERE ") ||
            containsCi(trimmed, L" VALUES ") || containsCi(trimmed, L" SET ")) {
            return true;
        }
    }
    return false;
}

int main() {
    std::wstring sqlQuery1 = L"select u.id, u.email, u.first_name, u.last_name, u.created_at, u.updated_at from users u where u.active = 1 and u.status = 'VERIFIED' and u.created_at >= '2023-01-01' order by u.created_at desc;";
    std::wstring sqlQuery2 = L"insert into audit_logs (id, user_id, action_type, description, ip_address, created_at) values (10023, 4059, 'USER_LOGIN_SUCCESS', 'User logged in successfully from web interface', '192.168.1.1', NOW());";
    std::wstring sqlQuery3 = L"update user_settings set theme = 'dark', notifications_enabled = 1, language = 'en-US' where user_id = 4059 and tenant_id = 12;";
    std::wstring sqlQuery4 = L"CREATE TABLE example_table (id INT PRIMARY KEY, name VARCHAR(255) NOT NULL, description TEXT, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP);";

    std::vector<std::wstring> queries = {sqlQuery1, sqlQuery2, sqlQuery3, sqlQuery4};

    // Correctness checks
    for (const auto& q : queries) {
        bool resOrig = IsSQLOriginal(q);
        if (IsSQLFastAscii(q) != resOrig || IsSQLUpperBuffer(q) != resOrig || IsSQLStdSearch(q) != resOrig) {
            std::cerr << "Mismatch on query: " << std::string(q.begin(), q.end()) << std::endl;
            return 1;
        }
    }
    std::cout << "All correctness checks passed!" << std::endl;

    const int iterations = 1000000;

    auto testFunc = [&](const char* name, auto func) {
        auto start = std::chrono::high_resolution_clock::now();
        volatile bool dummy = false;
        for (int i = 0; i < iterations; ++i) {
            for (const auto& q : queries) {
                dummy = dummy ^ func(q);
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> ms = end - start;
        double nsPerQuery = (ms.count() * 1e6) / (iterations * queries.size());
        std::cout << name << ": " << ms.count() << " ms (" << nsPerQuery << " ns/query)\n";
        return ms.count();
    };

    double origTime = testFunc("IsSQLOriginal  ", IsSQLOriginal);
    double asciiTime = testFunc("IsSQLFastAscii ", IsSQLFastAscii);
    double upperTime = testFunc("IsSQLUpperBuffer", IsSQLUpperBuffer);
    double searchTime = testFunc("IsSQLStdSearch ", IsSQLStdSearch);

    std::cout << "\nSpeedups over original:\n";
    std::cout << "FastAscii:   " << (origTime - asciiTime) / origTime * 100.0 << "%\n";
    std::cout << "UpperBuffer: " << (origTime - upperTime) / origTime * 100.0 << "%\n";
    std::cout << "StdSearch:   " << (origTime - searchTime) / origTime * 100.0 << "%\n";

    return 0;
}
