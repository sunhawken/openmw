#include <components/esm3/loaddial.hpp>

#include "../mwbase/environment.hpp"

#include "../mwworld/esmstore.hpp"
#include "../mwworld/store.hpp"

#include "keywordsearch.hpp"

#include "hypertextparser.hpp"

namespace MWDialogue
{
    namespace HyperTextParser
    {
        std::vector<Token> parseHyperText(const std::string& text, const MWDialogue::KeywordSearch<std::string>& search)
        {
            std::vector<Token> result;
            size_t posEnd = std::string::npos;
            size_t iterationPos = 0;

            auto tokenizeKeywords = [&](std::string::const_iterator begin, std::string::const_iterator end) {
                std::vector<KeywordSearch<std::string>::Match> matches;
                search.highlightKeywords(begin, end, matches);
                for (const auto& match : matches)
                    result.emplace_back(match, Token::ImplicitKeyword);
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

                    KeywordSearch<std::string>::Match token;
                    token.mBeg = text.begin() + posBegin + 1;
                    token.mEnd = text.begin() + posEnd;
                    token.mValue = std::string(token.mBeg, token.mEnd);
                    result.emplace_back(token, Token::ExplicitLink);

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

            phrase = phrase.substr(0, phrase.length() - pseudoAsterisksCount);

            return pseudoAsterisksCount;
        }
    }
}
