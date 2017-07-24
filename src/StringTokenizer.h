/* This file is part of RTags (http://rtags.net).

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#ifndef StringTokenizer_h
#define StringTokenizer_h

#include <rct/String.h>
#include <rct/List.h>
#include <cctype>
#include <algorithm>

enum MatchResultType {
    NO_MATCH,
    WORD_BOUNDARY_MATCH,
    PREFIX_MATCH_CASE_INSENSITIVE,
    PREFIX_MATCH_CASE_SENSITIVE,
    EXACT_MATCH_CASE_INSENSITIVE,
    EXACT_MATCH_CASE_SENSITIVE
};

class CompletionCandidate
{
public:
    CompletionCandidate()
        : priority(-1)
    {
    }

    String name;
    String signature;
    String kind;
    String parent;
    String brief_comment;
    String annotation;
    int priority;
};

class MatchResult
{
public:
    MatchResult(MatchResultType t, CompletionCandidate *c)
        : type(t), candidate(c)
    {
    }

    MatchResultType type;
    CompletionCandidate *candidate;
};

class NoMatchResult : public MatchResult
{
public:
    NoMatchResult(CompletionCandidate *c)
        : MatchResult(NO_MATCH, c)
    {
    }
};

class PrefixResult : public MatchResult
{
public:
    PrefixResult(MatchResultType t, CompletionCandidate *c, size_t l)
        : MatchResult(t, c), prefix_length(l)
    {
    }

    size_t prefix_length;
};

class WordBoundaryMatchResult : public MatchResult
{
public:
    WordBoundaryMatchResult(CompletionCandidate *c, std::vector<size_t> &i)
        : MatchResult(WORD_BOUNDARY_MATCH, c), indices(i)
    {
    }

    std::vector<size_t> indices;
};

struct MatchResultComparator {
    bool operator()(MatchResult *a, MatchResult *b)
    {
        if (a->type != b->type)
            return a->type > b->type;

        if (a->type == WORD_BOUNDARY_MATCH) {
            WordBoundaryMatchResult *wba = static_cast<WordBoundaryMatchResult *>(a);
            WordBoundaryMatchResult *wbb = static_cast<WordBoundaryMatchResult *>(b);

            for (size_t i = 0; i < wba->indices.size(); i++)
                if (wba->indices[i] != wbb->indices[i])
                    return wba->indices[i] > wbb->indices[i];
        }

        if (a->candidate->priority != b->candidate->priority)
            return a->candidate->priority < b->candidate->priority;

        return a < b;
    }
};

class StringTokenizer
{
public:
    inline std::vector<String> break_parts_of_word(const String &str);
    inline size_t common_prefix(const String &str1, const String &str2);
    inline MatchResult *find_match(CompletionCandidate *candidate, const String &query);
    inline bool is_boundary_match(const std::vector<String> &parts, const String &query, std::vector<size_t> &indices);
    inline String find_identifier_prefix(const String &line, size_t column, size_t *start);
    inline std::vector<MatchResult *> find_and_sort_matches(std::vector<CompletionCandidate *> &candidates, const String &query);

private:
    inline bool is_boundary_match(const std::vector<String> &parts,
                                  const String &query,
                                  std::vector<size_t> &indices,
                                  size_t query_start,
                                  size_t current_index);
};

std::vector<String> StringTokenizer::break_parts_of_word(const String &str)
{
    std::vector<String> result;
    String buffer;

    for (String::const_iterator c = str.begin(); c != str.end(); c++) {
        if (*c == '_') {
            /* Underscore symbol always break */
            if (!buffer.isEmpty()) {
                result.push_back(buffer);
                buffer.clear();
            }
        } else if (islower(*c)) {
            if (buffer.length() > 1 && isupper(buffer[buffer.length() - 1])) {
                /* Break: XML|Do.  */
                size_t l = buffer.length();
                result.push_back(buffer.mid(0, l - 1));
                buffer = buffer.mid(l - 1, 1);
            } else if (!buffer.isEmpty() && isdigit(buffer[buffer.length() - 1])) {
                /* Break: 0|D.  */
                result.push_back(buffer);
                buffer.clear();
            }

            buffer += *c;
        } else if (isupper(*c)) {
            /* Break: a|D or 0|D.  */
            if (!buffer.isEmpty() && !isupper(buffer[buffer.length() - 1])) {
                result.push_back(buffer);
                buffer.clear();
            }

            buffer += *c;
        } else if (isdigit(*c)) {
            /* Break: a|0 or A|0.  */
            if (!buffer.isEmpty() && !isdigit(buffer[buffer.length() - 1])) {
                result.push_back(buffer);
                buffer.clear();
            }

            buffer += *c;
        }
    }

    if (!buffer.isEmpty())
        result.push_back(buffer);

    /* Lower all parts of result.  */
    for (size_t i = 0; i < result.size(); i++)
        result[i].lowerCase();

    return result;
}

size_t StringTokenizer::common_prefix(const String &str1, const String &str2)
{
    size_t l = std::min(str1.length(), str2.length());

    for (size_t i = 0; i < l; i++)
        if (str1[i] != str2[i])
            return i;

    return l;
}

MatchResult *StringTokenizer::find_match(CompletionCandidate *candidate, const String &query)
{
    String c = candidate->name;

    if (query.length() > c.length())
        return new NoMatchResult(candidate);

    String c_lower = c.toLower();
    String query_lower = query.toLower();

    bool are_equal = c.length() == query.length();
    if (equal(query.begin(), query.end(), c.begin()))
        return new PrefixResult(are_equal ? EXACT_MATCH_CASE_SENSITIVE : PREFIX_MATCH_CASE_SENSITIVE, candidate, query.length());

    if (equal(query_lower.begin(), query_lower.end(), c_lower.begin()))
        return new PrefixResult(are_equal ? EXACT_MATCH_CASE_INSENSITIVE : PREFIX_MATCH_CASE_INSENSITIVE, candidate, query.length());

    std::vector<String> words = StringTokenizer().break_parts_of_word(c);
    std::vector<size_t> indices;
    bool r = is_boundary_match(words, query_lower, indices);
    if (r)
        return new WordBoundaryMatchResult(candidate, indices);

    return new NoMatchResult(candidate);
}

static bool isnotalnum(char c)
{
    return !isalnum(c);
}

bool StringTokenizer::is_boundary_match(const std::vector<String> &parts, const String &query, std::vector<size_t> &indices)
{
    /* Strip non-alphanum characters from candidate.  */
    std::string stripped = query;
    stripped.erase(std::remove_if(stripped.begin(), stripped.end(), isnotalnum), stripped.end());

    indices.resize(parts.size());
    return is_boundary_match(parts, stripped, indices, 0, 0);
}

bool StringTokenizer::is_boundary_match(const std::vector<String> &parts,
                                        const String &query,
                                        std::vector<size_t> &indices,
                                        size_t query_start,
                                        size_t current_index)
{
    if (query_start == query.length())
        return true;
    else if (current_index == parts.size())
        return false;

    String to_find = query.mid(query_start, query.length() - query_start);
    size_t longest_prefix = common_prefix(parts[current_index], to_find);

    for (int i = longest_prefix; i >= 0; i--) {
        indices[current_index] = i;
        bool r = is_boundary_match(parts, query, indices, query_start + i, current_index + 1);
        if (r)
            return r;
    }

    return false;
}

std::vector<MatchResult *> StringTokenizer::find_and_sort_matches(std::vector<CompletionCandidate *> &candidates, const String &query)
{
    std::vector<MatchResult *> results;

    for (std::vector<CompletionCandidate *>::const_iterator c = candidates.begin(); c != candidates.end(); c++) {
        MatchResult *r = find_match(*c, query);
        if (r->type != NO_MATCH)
            results.push_back(r);
        else
            delete r;
    }

    sort(results.begin(), results.end(), MatchResultComparator());

    return results;
}

#endif