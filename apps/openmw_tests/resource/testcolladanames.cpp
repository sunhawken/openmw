#include <components/resource/colladanames.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <sstream>
#include <vector>

namespace
{
    using namespace Resource;

    constexpr std::string_view sDocument = R"(<COLLADA>
  <library_visual_scenes><visual_scene id="scene">
    <node id="j_root" sid="j_root" name="rig_root" type="JOINT">
      <node id="j_a" name="arm_l" type="JOINT"/>
      <node
          id='j_b'
          name='arm l' type='JOINT'/>
      <node id="j_dot" name="spine.upper &amp; co" type="JOINT"/>
      <node id="Armature_Bip01_Spine1" name="Bip01_Spine1" type="JOINT"/>
      <node id="Armature_Weapon_Bone" name="Weapon_Bone" type="JOINT"/>
      <node id="noname" type="JOINT"/>
    </node>
  </visual_scene></library_visual_scenes>
  <library_animation_clips>
    <animation_clip id="clip0" name="wave_hello" start="0" end="1"/>
    <animation_clip id="clip1" name="Slash Combo.2" start="1" end="2"/>
  </library_animation_clips>
  <node_extra id="x" name="ignored"/>
</COLLADA>)";

    TEST(ResourceColladaNamesTest, readsNodeAndClipNames)
    {
        const ColladaNames names = parseColladaNames(sDocument);
        EXPECT_EQ(names.mNodes.at("j_root"), "rig_root");
        EXPECT_EQ(names.mNodes.at("j_a"), "arm_l");
        EXPECT_EQ(names.mNodes.at("j_b"), "arm l");
        EXPECT_EQ(names.mNodes.at("j_dot"), "spine.upper & co");
        EXPECT_EQ(names.mNodes.count("noname"), 0u);
        EXPECT_EQ(names.mNodes.count("x"), 0u);
        EXPECT_EQ(names.mClips.at("clip0"), "wave_hello");
        EXPECT_EQ(names.mClips.at("clip1"), "Slash Combo.2");
    }

    TEST(ResourceColladaNamesTest, keepsExactNamesWithoutCollisions)
    {
        const ColladaNames names = parseColladaNames(sDocument);
        EXPECT_EQ(colladaNodeName(names, "j_root"), "rig_root");
        EXPECT_EQ(colladaNodeName(names, "j_a"), "arm_l"); // not turned into "arm l"
        EXPECT_EQ(colladaNodeName(names, "j_b"), "arm l");
        EXPECT_EQ(colladaNodeName(names, "j_dot"), "spine.upper & co");
    }

    TEST(ResourceColladaNamesTest, explicitNamesKeepUnderscores)
    {
        const ColladaNames names = parseColladaNames(sDocument);
        EXPECT_EQ(colladaNodeName(names, "Armature_Bip01_Spine1"), "Bip01_Spine1");
        EXPECT_EQ(colladaNodeName(names, "Armature_Weapon_Bone"), "Weapon_Bone");
    }

    TEST(ResourceColladaNamesTest, legacyFallbackOnlyWithoutCollision)
    {
        // "a_b" has no name: its spaced form "a b" is another node's name, so it stays.
        // "c_d" has no name and nothing is called "c d": the old rule applies.
        // "e_f" has no name, but another node's id is "e f".
        const ColladaNames names = parseColladaNames(
            R"(<node id="a_b"/><node id="n1" name="a b"/><node id="c_d"/><node id="e_f"/><node id="e f"/>)");
        EXPECT_EQ(colladaNodeName(names, "a_b"), "a_b");
        EXPECT_EQ(colladaNodeName(names, "n1"), "a b");
        EXPECT_EQ(colladaNodeName(names, "c_d"), "c d");
        EXPECT_EQ(colladaNodeName(names, "e_f"), "e_f");
    }

    TEST(ResourceColladaNamesTest, duplicateIdsKeepFirstName)
    {
        const ColladaNames names = parseColladaNames(R"(<node id="x" name="first"/><node id="x" name="second"/>)");
        EXPECT_EQ(colladaNodeName(names, "x"), "first");
    }

    TEST(ResourceColladaNamesTest, numericEntitiesDecode)
    {
        const ColladaNames names = parseColladaNames(R"(<node id="x" name="a&#32;b&#x2e;c &lt;d&gt;"/>)");
        EXPECT_EQ(colladaNodeName(names, "x"), "a b.c <d>");
    }

    TEST(ResourceColladaNamesTest, filesWithoutNamesKeepOldRule)
    {
        const ColladaNames names = parseColladaNames(R"(<node id="Bip01_L_Hand"/>)");
        EXPECT_TRUE(names.mNodes.empty());
        EXPECT_EQ(colladaNodeName(names, "Bip01_L_Hand"), "Bip01 L Hand");
        EXPECT_EQ(colladaNodeName(names, "some_bone"), "some bone");
        EXPECT_EQ(colladaClipName(names, "Default"), "Default");
    }

    TEST(ResourceColladaNamesTest, readerTextIsNulTerminatedAndComplete)
    {
        // Regression: OSG copies exactly the stream length and parses the copy as a
        // C string. It must end in NUL and keep every byte of the document.
        const std::string document = "<COLLADA><node id=\"a\" name=\"b\"/></COLLADA>";
        std::istringstream in(document);
        const std::string text = readColladaForReader(in);
        ASSERT_EQ(text.size(), document.size() + 1);
        EXPECT_EQ(text.back(), '\0');
        EXPECT_EQ(text.compare(0, document.size(), document), 0);

        // What OSG does with the stream: length from seek to end, exact copy,
        // then use as a C string. Fill the memory after the copy with non-NUL
        // bytes first, as a reused heap block would have.
        std::istringstream readerInput(text);
        readerInput.seekg(0, std::ios::end);
        const std::size_t length = static_cast<std::size_t>(readerInput.tellg());
        readerInput.seekg(0, std::ios::beg);
        std::vector<char> memory(length + 16, 'x');
        readerInput.read(memory.data(), static_cast<std::streamsize>(length));
        EXPECT_EQ(std::string(memory.data()), document);

        // Without the NUL the same copy runs into the following bytes.
        std::istringstream unterminated(document);
        std::vector<char> memory2(document.size() + 16, 'x');
        unterminated.read(memory2.data(), static_cast<std::streamsize>(document.size()));
        EXPECT_NE(std::string(memory2.data(), strnlen(memory2.data(), memory2.size())), document);
    }
}
