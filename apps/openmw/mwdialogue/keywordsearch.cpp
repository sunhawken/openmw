#include "keywordsearch.hpp"

#include <algorithm>
#include <stdexcept>

#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>

namespace MWDialogue
{
    void KeywordSearch::seed(std::string_view keyword, std::string_view value)
    {
        if (keyword.empty())
            return;
        buildTrie(keyword, value, 0, mRoot);
    }

    void KeywordSearch::clear()
    {
        mRoot.mChildren.clear();
        mRoot.mKeyword.clear();
    }

    void KeywordSearch::highlightKeywords(Point beg, Point end, std::vector<Match>& out) const
    {
        std::vector<Match> matches;
        for (Point i = beg; i != end; ++i)
        {
            if (i != beg)
            {
                Point prev = i;
                --prev;
                constexpr std::string_view wordSeparators = "\n\r \t'\"";
                if (wordSeparators.find(*prev) == std::string_view::npos)
                    continue;
            }

            const Entry* current = &mRoot;
            for (Point it = i; it != end; ++it)
            {
                auto found = current->mChildren.find(Misc::StringUtils::toLower(*it));
                if (found == current->mChildren.end())
                    break;
                current = &found->second;
                if (!current->mKeyword.empty())
                {
                    std::string_view remainingText(it + 1, end);
                    std::string_view remainingKeyword = std::string_view(current->mKeyword).substr(it + 1 - i);
                    if (Misc::StringUtils::ciStartsWith(remainingText, remainingKeyword))
                    {
                        Match match;
                        match.mValue = current->mValue;
                        match.mBeg = i;
                        match.mEnd = i + current->mKeyword.size();
                        matches.push_back(match);
                    }
                }
            }
        }
        // resolve overlapping keywords
        while (!matches.empty())
        {
            std::size_t longestKeywordSize = 0;
            auto longestKeyword = matches.begin();
            for (auto it = matches.begin(); it != matches.end(); ++it)
            {
                std::size_t size = it->mEnd - it->mBeg;
                if (size > longestKeywordSize)
                {
                    longestKeywordSize = size;
                    longestKeyword = it;
                }

                auto next = it + 1;

                if (next == matches.end())
                    break;

                if (it->mEnd <= next->mBeg)
                {
                    break; // no overlap
                }
            }

            Match keyword = *longestKeyword;
            matches.erase(longestKeyword);
            out.push_back(keyword);
            // erase anything that overlaps with the keyword we just added to the output
            std::erase_if(
                matches, [&](const Match& match) { return match.mBeg < keyword.mEnd && match.mEnd > keyword.mBeg; });
        }

        std::sort(out.begin(), out.end(), [](const Match& left, const Match& right) { return left.mBeg < right.mBeg; });
    }

    void KeywordSearch::buildTrie(std::string_view keyword, std::string_view value, size_t depth, Entry& entry)
    {
        const char ch = Misc::StringUtils::toLower(keyword[depth]);
        const auto found = entry.mChildren.find(ch);

        if (found == entry.mChildren.end())
        {
            entry.mChildren[ch].mValue = std::string(value);
            entry.mChildren[ch].mKeyword = std::string(keyword);
        }
        else
        {
            if (!found->second.mKeyword.empty())
            {
                std::string_view existingKeyword = found->second.mKeyword;
                if (Misc::StringUtils::ciEqual(keyword, existingKeyword))
                    throw std::runtime_error("duplicate keyword inserted");
                if (depth >= existingKeyword.size())
                    throw std::runtime_error("unexpected trie depth");
                // Turn this Entry into a branch and append a leaf to hold its current value
                if (depth + 1 < existingKeyword.size())
                {
                    buildTrie(existingKeyword, found->second.mValue, depth + 1, found->second);
                    found->second.mKeyword.clear();
                }
            }
            if (depth + 1 == keyword.size())
            {
                found->second.mValue = std::string(value);
                found->second.mKeyword = std::string(keyword);
            }
            else
            {
                buildTrie(keyword, value, depth + 1, found->second);
            }
        }
    }

    std::vector<KeywordSearch::Match> KeywordSearch::parseHyperText(const std::string& text) const
    {
        std::vector<Match> matches;
        size_t posEnd = std::string::npos;
        size_t iterationPos = 0;

        for (;;)
        {
            const size_t posBegin = text.find('@', iterationPos);
            if (posBegin != std::string::npos)
                posEnd = text.find('#', posBegin);

            if (posBegin != std::string::npos && posEnd != std::string::npos)
            {
                if (posBegin != iterationPos)
                    highlightKeywords(text.begin() + iterationPos, text.begin() + posBegin, matches);

                Match token;
                token.mExplicit = true;

                // This covers the entire link including the tags
                token.mBeg = text.begin() + posBegin;
                token.mEnd = text.begin() + posEnd + 1;

                // This is the translation-ready line with the tags excluded
                token.mValue = std::string(token.mBeg + 1, token.mEnd - 1);
                size_t asteriskCount = removePseudoAsterisks(token.mValue);
                for (; asteriskCount > 0; --asteriskCount)
                    token.mValue.append("*");

                matches.push_back(token);

                iterationPos = posEnd + 1;
            }
            else
            {
                if (iterationPos < text.size())
                    highlightKeywords(text.begin() + iterationPos, text.end(), matches);
                break;
            }
        }

        return matches;
    }

    std::string KeywordSearch::Match::getDisplayName() const
    {
        if (mExplicit)
        {
            std::string displayName = std::string(mBeg + 1, mEnd - 1);
            removePseudoAsterisks(displayName);
            return displayName;
        }
        return std::string(mBeg, mEnd);
    }

    size_t removePseudoAsterisks(std::string& phrase)
    {
        size_t pseudoAsterisksCount = 0;

        if (!phrase.empty())
        {
            std::string::reverse_iterator rit = phrase.rbegin();
            const char specialPseudoAsteriskCharacter = 127;

            while (rit != phrase.rend() && *rit == specialPseudoAsteriskCharacter)
            {
                pseudoAsterisksCount++;
                ++rit;
            }
        }

        if (pseudoAsterisksCount > 0)
        {
            phrase = phrase.substr(0, phrase.length() - pseudoAsterisksCount);
        }

        return pseudoAsterisksCount;
    }
}
