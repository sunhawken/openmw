#include "hypertextparser.hpp"

namespace MWDialogue
{
    namespace HyperTextParser
    {
        std::vector<Token> parseHyperText(const std::string& text, const KeywordSearch& search)
        {
            std::vector<Token> result;
            size_t posEnd = std::string::npos;
            size_t iterationPos = 0;

            auto tokenizeKeywords = [&](std::string::const_iterator begin, std::string::const_iterator end) {
                std::vector<KeywordSearch::Match> matches;
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

                    KeywordSearch::Match token;
                    token.mBeg = text.begin() + posBegin + 1;
                    token.mEnd = text.begin() + posEnd;
                    token.mValue = std::string(token.mBeg, token.mEnd);

                    // Some post-processing for easier standard form conversion
                    size_t asteriskCount = removePseudoAsterisks(token.mValue);
                    for (; asteriskCount > 0; --asteriskCount)
                        token.mValue.append("*");

                    result.emplace_back(token, true);

                    iterationPos = posEnd + 1;
                }
                else
                {
                    if (iterationPos < text.size())
                        tokenizeKeywords(text.begin() + iterationPos, text.end());
                    break;
                }
            }

            return result;
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
}
