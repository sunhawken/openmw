#include "apps/openmw/mwdialogue/keywordsearch.hpp"

#include <gtest/gtest.h>

struct KeywordSearchTest : public ::testing::Test
{
protected:
    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(KeywordSearchTest, keyword_test_conflict_resolution)
{
    // test to make sure the longest keyword in a chain of conflicting keywords gets chosen
    MWDialogue::KeywordSearch search;
    search.seed("foo bar", {});
    search.seed("bar lock", {});
    search.seed("lock switch", {});

    std::string text = "foo bar lock switch";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    // Should contain: "foo bar", "lock switch"
    EXPECT_EQ(matches.size(), 2);
    EXPECT_EQ(std::string(matches.front().mBeg, matches.front().mEnd), "foo bar");
    EXPECT_EQ(std::string(matches.rbegin()->mBeg, matches.rbegin()->mEnd), "lock switch");
}

TEST_F(KeywordSearchTest, keyword_test_conflict_resolution2)
{
    MWDialogue::KeywordSearch search;
    search.seed("the dwemer", {});
    search.seed("dwemer language", {});

    std::string text = "the dwemer language";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), 1);
    EXPECT_EQ(std::string(matches.front().mBeg, matches.front().mEnd), "dwemer language");
}

TEST_F(KeywordSearchTest, keyword_test_conflict_resolution3)
{
    // Test that the longest keyword is chosen, rather than maximizing the
    // amount of highlighted characters by highlighting the first and last keyword
    MWDialogue::KeywordSearch search;
    search.seed("foo bar", {});
    search.seed("bar lock", {});
    search.seed("lock so", {});

    std::string text = "foo bar lock so";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), 1);
    EXPECT_EQ(std::string(matches.front().mBeg, matches.front().mEnd), "bar lock");
}

TEST_F(KeywordSearchTest, keyword_test_utf8_word_begin)
{
    // Make sure that the search works well on UTF-8 strings containing some non-ASCII (French)
    MWDialogue::KeywordSearch search;
    search.seed("états", {});
    search.seed("ïrradiés", {});
    search.seed("ça nous déçois", {});
    search.seed("nous", {});

    std::string text
        = "les nations unis ont réunis le monde entier, états units inclus pour parler du problème des gens ïrradiés "
          "et ça nous déçois";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), 3);
    EXPECT_EQ(std::string(matches[0].mBeg, matches[0].mEnd), "états");
    EXPECT_EQ(std::string(matches[1].mBeg, matches[1].mEnd), "ïrradiés");
    EXPECT_EQ(std::string(matches[2].mBeg, matches[2].mEnd), "ça nous déçois");
}

TEST_F(KeywordSearchTest, keyword_test_non_alpha_non_whitespace_word_begin)
{
    // Make sure that the search works well even if the separator is not whitespace
    MWDialogue::KeywordSearch search;
    search.seed("Report to caius cosades", {});

    std::string text = "I was told to \"Report to Caius Cosades\"";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), 1);
    EXPECT_EQ(std::string(matches[0].mBeg, matches[0].mEnd), "Report to Caius Cosades");
}

TEST_F(KeywordSearchTest, keyword_test_russian_ascii_before)
{
    // Make sure that the search works well even if the separator is not whitespace with Russian chars
    MWDialogue::KeywordSearch search;
    search.seed("Доложить Каю Косадесу", {});

    std::string text
        = "Что? Да. Я Кай Косадес. То есть как это, вам велели 'Доложить Каю Косадесу'? О чем вы говорите?";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), 1);
    EXPECT_EQ(std::string(matches[0].mBeg, matches[0].mEnd), "Доложить Каю Косадесу");
}

TEST_F(KeywordSearchTest, keyword_test_substrings_without_word_separators)
{
    // Make sure that the search does not highlight substrings within words
    // i.e. "Force" does not contain "orc"
    // and "bring" does not contain "ring"
    MWDialogue::KeywordSearch search;
    search.seed("orc", {});
    search.seed("ring", {});

    std::string text = "Bring the Force, Lucan!";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), {});
}

TEST_F(KeywordSearchTest, keyword_test_initial_substrings_match)
{
    // Make sure that the search highlights prefix substrings
    // "Orcs" should match "orc"
    // "ring" is not matched because "-" is not a word separator
    MWDialogue::KeywordSearch search;
    search.seed("orc", {});
    search.seed("ring", {});

    std::string text = "Bring the Orcs some gold-rings.";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), 1);
    EXPECT_EQ(std::string(matches[0].mBeg, matches[0].mEnd), "Orc");
}

TEST_F(KeywordSearchTest, keyword_test_french_substrings)
{
    // Substrings within words should not match
    MWDialogue::KeywordSearch search;
    search.seed("ages", {});
    search.seed("orc", {});

    std::string text = "traçages et forces";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), {});
}

TEST_F(KeywordSearchTest, keyword_test_single_char_strings)
{
    // It should be possible to match a single character
    MWDialogue::KeywordSearch search;
    search.seed("AB", {});
    search.seed("a", {});

    std::string text = "a ab";

    std::vector<MWDialogue::KeywordSearch::Match> matches;
    search.highlightKeywords(text.begin(), text.end(), matches);

    EXPECT_EQ(matches.size(), 2);
    EXPECT_EQ(std::string(matches[0].mBeg, matches[0].mEnd), "a");
    EXPECT_EQ(std::string(matches[1].mBeg, matches[1].mEnd), "ab");
}
