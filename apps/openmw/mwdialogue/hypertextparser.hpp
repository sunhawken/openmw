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
            enum Type
            {
                ExplicitLink, // enclosed in @#
                ImplicitKeyword
            };

            Token(const KeywordSearch<std::string>::Match& token, Type type)
                : mMatch(token)
                , mType(type)
            {
            }

            bool isExplicitLink() { return mType == ExplicitLink; }

            KeywordSearch<std::string>::Match mMatch;
            Type mType;
        };

        std::vector<Token> parseHyperText(
            const std::string& text, const MWDialogue::KeywordSearch<std::string>& search);
        size_t removePseudoAsterisks(std::string& phrase);
    }
}

#endif
