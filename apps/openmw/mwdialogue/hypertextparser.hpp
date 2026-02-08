#ifndef GAME_MWDIALOGUE_HYPERTEXTPARSER_H
#define GAME_MWDIALOGUE_HYPERTEXTPARSER_H

#include <string>
#include <vector>

#include "../mwdialogue/keywordsearch.hpp"

namespace MWDialogue
{
    namespace HyperTextParser
    {
        struct Token
        {
            KeywordSearch::Match mMatch;
            bool mIsExplicit;

            Token(KeywordSearch::Match match, bool isExplicit)
                : mMatch(match)
                , mIsExplicit(isExplicit)
            {}
        };

        std::vector<Token> parseHyperText(const std::string& text, const KeywordSearch& search);
        size_t removePseudoAsterisks(std::string& phrase);
    }
}

#endif
