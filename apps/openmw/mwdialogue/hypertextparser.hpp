#ifndef GAME_MWDIALOGUE_HYPERTEXTPARSER_H
#define GAME_MWDIALOGUE_HYPERTEXTPARSER_H

#include <string>
#include <type_traits>
#include <vector>

#include "../mwdialogue/keywordsearch.hpp"

namespace MWDialogue
{
    namespace HyperTextParser
    {
        template <typename T>
        struct Token
        {
            using MatchType = typename KeywordSearch<T>::Match;

            Token(const MatchType& token, bool isExplicit)
                : mMatch(token)
                , mIsExplicit(isExplicit)
            {
            }

            MatchType mMatch;
            bool mIsExplicit;
        };

        template <typename T>
        std::vector<Token<T>> parseHyperText(const std::string& text, const KeywordSearch<T>& search)
        {
            std::vector<Token<T>> result;
            size_t posEnd = std::string::npos;
            size_t iterationPos = 0;

            auto tokenizeKeywords = [&](std::string::const_iterator begin, std::string::const_iterator end) {
                std::vector<typename KeywordSearch<T>::Match> matches;
                search.highlightKeywords(begin, end, matches);
                for (const auto& match : matches)
                    result.emplace_back(match, false);
            };

            for (;;)
            {
                const size_t posBegin = text.find('@', iterationPos);
                if (posBegin != std::string::npos)
                    posEnd = text.find('#', posBegin);

                if (posBegin != std::string::npos && posEnd != std::string::npos)
                {
                    if (posBegin != iterationPos)
                        tokenizeKeywords(text.begin() + iterationPos, text.begin() + posBegin);

                    typename KeywordSearch<T>::Match token;
                    token.mBeg = text.begin() + posBegin + 1;
                    token.mEnd = text.begin() + posEnd;
                    // No value to use. The caller must figure out what to do with the range.
                    token.mValue = {};

                    result.emplace_back(token, true);

                    iterationPos = posEnd + 1;
                }
                else
                {
                    if (iterationPos != text.size())
                        tokenizeKeywords(text.begin() + iterationPos, text.end());
                    break;
                }
            }

            return result;
        }

        inline size_t removePseudoAsterisks(std::string& phrase)
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

            phrase = phrase.substr(0, phrase.length() - pseudoAsterisksCount);

            return pseudoAsterisksCount;
        }
    }
}

#endif
