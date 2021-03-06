#ifndef word_db_hh_INCLUDED
#define word_db_hh_INCLUDED

#include "buffer.hh"
#include "shared_string.hh"
#include "unordered_map.hh"
#include "vector.hh"

#include <bitset>

namespace Kakoune
{

using UsedLetters = std::bitset<64>;
UsedLetters used_letters(StringView str);

// maintain a database of words available in a buffer
class WordDB
{
public:
    WordDB(const Buffer& buffer);
    WordDB(const WordDB&) = delete;
    WordDB(WordDB&&) = default;

    using WordList = Vector<StringView>;
    template<typename MatchFunc>
    WordList find_matching(StringView str, MatchFunc match)
    {
        update_db();
        const UsedLetters letters = used_letters(str);
        WordList res;
        for (auto&& word : m_words)
        {
            if ((letters & word.second.letters) == letters and
                match(word.first, str))
                res.push_back(word.first);
        }
        return res;
    }

    int get_word_occurences(StringView word) const;
private:
    void update_db();
    void add_words(const WordList& words);
    void remove_words(const WordList& words);

    struct WordInfo
    {
        UsedLetters letters;
        int refcount;
    };
    using WordToInfo = UnorderedMap<SharedString, WordInfo, MemoryDomain::WordDB>;
    using Lines = Vector<ref_ptr<StringStorage>, MemoryDomain::WordDB>;

    safe_ptr<const Buffer> m_buffer;
    size_t m_timestamp;
    WordToInfo m_words;
    Lines m_lines;
};

}

#endif // word_db_hh_INCLUDED
