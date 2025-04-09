/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022, 2023, 2024, 2025
              Vladimír Vondruš <mosra@centrum.cz>

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.
*/

#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/StridedArrayView.h>
#include <Corrade/PluginManager/Manager.h>
#include <Corrade/TestSuite/Tester.h>
#include <Corrade/TestSuite/Compare/Container.h>
#include <Corrade/Utility/Endianness.h>
#include <Corrade/Utility/Path.h>
#include <Magnum/Math/Vector2.h>
#include <Magnum/Text/AbstractFont.h>
#include <Magnum/Text/AbstractShaper.h>
#include <Magnum/Text/Direction.h>
#include <Magnum/Text/Feature.h>
#include <Magnum/Text/Script.h>
#include <hb.h>

#include "configure.h"

namespace Magnum { namespace Text { namespace Test { namespace {

struct HarfBuzzFontTest: TestSuite::Tester {
    explicit HarfBuzzFontTest();

    void scriptMapping();

    void shape();
    void shapeDifferentScriptLanguageDirection();
    void shapeAutodetectScriptLanguageDirection();
    void shapeUnsupportedScript();
    void shapeEmpty();
    void shapeGlyphOffset();

    void shapeMultiple();
    void shapeMultipleAutodetection();

    void shapeFeatures();

    void openTwice();

    /* Explicitly forbid system-wide plugin dependencies */
    PluginManager::Manager<AbstractFont> _manager{"nonexistent"};
};

const struct {
    const char* name;
    const char* string;
    UnsignedInt eGlyphId, eGlyphClusterExtraSize;
    UnsignedInt begin, end;
    Float advanceAfterW, advanceAfterE;
} ShapeData[]{
    {"", "Weave", 72, 0, 0, ~UnsignedInt{},
        /* HarfBuzz between 1.7 and 3.1 gives a slightly different value */
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 || \
            HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301
        16.3125f,
        #else
        16.2969f,
        #endif
        8.25f},
    {"substring", "haWeavefefe", 72, 0, 2, 7,
        /* HarfBuzz between 1.7 and 3.1 gives a slightly different value */
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 || \
            HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301
        16.3125f,
        #else
        16.2969f,
        #endif
        8.25f},
    /* `Wě` has slightly different spacing than `We` but there it doesn't get
       different between versions at least */
    {"UTF-8", "Wěave", 220, 1, 0, ~UnsignedInt{}, 16.6562f, 8.34375f},
    {"UTF-8 substring", "haWěavefefe", 220, 1, 2, 8, 16.6562f, 8.34375f},
    /* Without the literal split it says "Unicode sequence out of range". Wow,
       who designed this mess?! That's even worse than octal literals. */
    {"UTF-8, decomposed", "We\xcc\x8c" "ave", 220, 2, 0, ~UnsignedInt{}, 16.6562f, 8.34375f},
    /* Decomposing to actual `e` and `ˇ` tested in shapeGlyphOffset() */
};

const struct {
    const char* name;
    ShapeDirection direction;
    bool flip;
} ShapeDifferentScriptLanguageDirectionData[]{
    {"left to right", ShapeDirection::LeftToRight, false},
    {"right to left", ShapeDirection::RightToLeft, true},
    {"top to bottom", ShapeDirection::TopToBottom, false},
    {"bottom to top", ShapeDirection::BottomToTop, true},
};

const struct {
    const char* name;
    bool explicitlySetUnspecified;
} ShapeAutodetectScriptLanguageDirectionData[]{
    {"", false},
    {"explicitly set unspecified values", true}
};

const struct {
    const char* name;
    bool reuse;
} ShapeMultipleData[]{
    {"new shaper every time", false},
    {"reuse previous shaper", false},
};

const struct {
    const char* name;
    Containers::Array<FeatureRange> features;
    Float advances[4];
} ShapeFeaturesData[]{
    {"none", {}, {
        /* Versions 3.3.0 and 3.3.1 reported {16.5f, 0.0f} here, but the
           change is reverted in 3.3.2 again "as it proved problematic". */
        16.3594f,
        8.26562f,
        /* HarfBuzz before 1.7 and after 3.1 gives 8.0, versions between the
           other */
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 || \
            HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301
        8.0f,
        #else
        7.984384f,
        #endif
        8.34375f
    }},
    {"no-op", {InPlaceInit, {
        /* These are enabled by HarfBuzz by default */
        Feature::Kerning,
        Feature::StandardLigatures
    }}, {
        /* Same as above, as kerning is enabled by default */
        16.3594f,
        8.26562f,
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 || \
            HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301
        8.0f,
        #else
        7.984384f,
        #endif
        8.34375f
    }},
    {"kerning disabled and then enabled again", {InPlaceInit, {
        {Feature::Kerning, false},
        {Feature::Kerning, true}
    }}, {
        /* Should be the same as "none" */
        16.3594f,
        8.26562f,
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 || \
            HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301
        8.0f,
        #else
        7.984384f,
        #endif
        8.34375f
    }},
    {"kerning disabled", {InPlaceInit, {
        {Feature::Kerning, false}
    }}, {
        /* Not quite the same as what FreeTypeFont gives back, but different
           from above at least */
        16.6562f,
        8.26562f, /* same as with kerning */
        8.09375f,
        8.34375f  /* same as with kerning */
    }},
    {"kerning enabled and then disabled again", {InPlaceInit, {
        {Feature::Kerning, true},
        {Feature::Kerning, false},
    }}, {
        /* Should be the same as "kerning disabled" */
        16.6562f,
        8.26562f,
        8.09375f,
        8.34375f
    }},
    {"kerning enabled and disabled for a part", {InPlaceInit, {
        {Feature::Kerning, 0, 2, true},
        {Feature::Kerning, 2, 4, false},
    }}, {
        16.3594f, /* same as with kerning */
        8.26562f, /* same as with kerning */
        8.09375f,
        8.34375f  /* same as with kerning */
    }},
    {"kerning disabled and enabled for a part", {InPlaceInit, {
        /* Just different order from above, should result in the same */
        {Feature::Kerning, 2, 4, false},
        {Feature::Kerning, 0, 2, true},
    }}, {
        16.3594f,
        8.26562f,
        8.09375f,
        8.34375f
    }},
};

HarfBuzzFontTest::HarfBuzzFontTest() {
    addTests({&HarfBuzzFontTest::scriptMapping});

    addInstancedTests({&HarfBuzzFontTest::shape},
        Containers::arraySize(ShapeData));

    addInstancedTests({&HarfBuzzFontTest::shapeDifferentScriptLanguageDirection},
        Containers::arraySize(ShapeDifferentScriptLanguageDirectionData));

    addInstancedTests({&HarfBuzzFontTest::shapeAutodetectScriptLanguageDirection},
        Containers::arraySize(ShapeAutodetectScriptLanguageDirectionData));

    addTests({&HarfBuzzFontTest::shapeUnsupportedScript,
              &HarfBuzzFontTest::shapeEmpty,
              &HarfBuzzFontTest::shapeGlyphOffset});

    addInstancedTests({&HarfBuzzFontTest::shapeMultiple,
                       &HarfBuzzFontTest::shapeMultipleAutodetection},
        Containers::arraySize(ShapeMultipleData));

    addInstancedTests({&HarfBuzzFontTest::shapeFeatures},
        Containers::arraySize(ShapeFeaturesData));

    addTests({&HarfBuzzFontTest::openTwice});

    /* Load the plugin directly from the build tree. Otherwise it's static and
       already loaded. */
    #if defined(FREETYPEFONT_PLUGIN_FILENAME) && defined(HARFBUZZFONT_PLUGIN_FILENAME)
    CORRADE_INTERNAL_ASSERT_OUTPUT(_manager.load(FREETYPEFONT_PLUGIN_FILENAME) & PluginManager::LoadState::Loaded);
    CORRADE_INTERNAL_ASSERT_OUTPUT(_manager.load(HARFBUZZFONT_PLUGIN_FILENAME) & PluginManager::LoadState::Loaded);
    #endif
}

void HarfBuzzFontTest::scriptMapping() {
    /* The FourCC values should match between the Script enum and HarfBuzz to
       not need expensive mapping. Eh, actually, they don't match, as HB_TAG()
       creates an Endian-dependent value, so ntaL instead of Latn on Little
       Endian. I couldn't find any documentation or a bug report on why this
       differs from what OpenType fonts actually have (where it's Big-Endian
       always, i.e. Latn), apart from one "oops" in this old commit:
        https://github.com/harfbuzz/harfbuzz/commit/fcd6f5326166e993b8f5222efbaffe916da98f0a */
    CORRADE_COMPARE(UnsignedInt(Script::Unspecified), HB_SCRIPT_INVALID);
    #define _c(name, hb) CORRADE_COMPARE(UnsignedInt(Script::name), Utility::Endianness::bigEndian(HB_SCRIPT_ ## hb));
    #define _c_include_unsupported 0
    #include "../scriptMapping.h"
    #undef _c_include_unsupported
    #undef _c

    /* Verify the header indeed contains cases for all Script values. It's not
       guarded with -Werror=switch as that would mean a Magnum update adding a
       new Script value would break a build of HarfBuzzFont tests, which is
       undesirable. So it's just a warning. */
    Script script = Script::Unknown;
    switch(script) {
        case Script::Unspecified:
        #define _c(name, hb) case Script::name:
        #define _c_include_all 1
        #include "../scriptMapping.h"
        #undef _c_include_all
        #undef _c
            CORRADE_VERIFY(UnsignedInt(script));
    }

    /* Also verify that the header contains all hb_script_t values supported by
       this HarfBuzz version. Also not -Werror=switch as this could break on a
       HarfBuzz update. */
    hb_script_t hbScript = HB_SCRIPT_UNKNOWN;
    switch(hbScript) {
        case HB_SCRIPT_INVALID:
        #define _c(name, hb) case HB_SCRIPT_ ## hb:
        #define _c_include_supported 1
        #include "../scriptMapping.h"
        #undef _c_include_supported
        #undef _c
        case _HB_SCRIPT_MAX_VALUE:
        /* These two values used to be different before 2.0.0, not anymore:
            https://github.com/harfbuzz/harfbuzz/commit/90dd255e570bf8ea3436e2f29242068845256e55 */
        #if !HB_VERSION_ATLEAST(2, 0, 0)
        case _HB_SCRIPT_MAX_VALUE_SIGNED:
        #endif
            CORRADE_VERIFY(UnsignedInt(hbScript));
    }
}

void HarfBuzzFontTest::shape() {
    auto&& data = ShapeData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* There's no script / language / direction set by default */
    CORRADE_COMPARE(shaper->script(), Script::Unspecified);
    CORRADE_COMPARE(shaper->language(), "");
    CORRADE_COMPARE(shaper->direction(), ShapeDirection::Unspecified);

    /* Shape a text */
    CORRADE_VERIFY(shaper->setScript(Script::Latin));
    CORRADE_VERIFY(shaper->setLanguage("en"));
    CORRADE_VERIFY(shaper->setDirection(ShapeDirection::LeftToRight));
    CORRADE_COMPARE(shaper->shape(data.string, data.begin, data.end), 5);

    /* The script / language / direction set above should get used for
       shaping */
    CORRADE_COMPARE(shaper->script(), Script::Latin);
    CORRADE_COMPARE(shaper->language(), "en");
    CORRADE_COMPARE(shaper->direction(), ShapeDirection::LeftToRight);

    UnsignedInt ids[5];
    Vector2 offsets[5];
    Vector2 advances[5];
    UnsignedInt clusters[5];
    shaper->glyphIdsInto(ids);
    shaper->glyphOffsetsAdvancesInto(offsets, advances);
    shaper->glyphClustersInto(clusters);
    CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
        58u,            /* 'W' */
        data.eGlyphId,  /* 'e' or 'ě' */
        68u,            /* 'a' */
        89u,            /* 'v' */
        72u             /* 'e' */
    }), TestSuite::Compare::Container);
    /* Offsets aren't usually set to anything. Shaping that produces glyph
       offsets is tested in shapeGlyphOffset(). */
    CORRADE_COMPARE_AS(Containers::arrayView(offsets), Containers::arrayView<Vector2>({
        {}, {}, {}, {}, {}
    }), TestSuite::Compare::Container);
    CORRADE_COMPARE_AS(Containers::arrayView(advances), Containers::arrayView<Vector2>({
        {data.advanceAfterW, 0.0f},
        {data.advanceAfterE, 0.0f},
        {8.26562f, 0.0f},
        {HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 ||
         HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301 ? 8.0f : 7.984384f,
         0.0f},
        {8.34375f, 0.0f}
    }), TestSuite::Compare::Container);
    CORRADE_COMPARE_AS(Containers::arrayView(clusters), Containers::arrayView({
        data.begin + 0u,
        data.begin + 1u,
        data.begin + 2u + data.eGlyphClusterExtraSize,
        data.begin + 3u + data.eGlyphClusterExtraSize,
        data.begin + 4u + data.eGlyphClusterExtraSize,
    }), TestSuite::Compare::Container);
}

void HarfBuzzFontTest::shapeDifferentScriptLanguageDirection() {
    auto&& data = ShapeDifferentScriptLanguageDirectionData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    CORRADE_VERIFY(shaper->setScript(Script::Greek));
    CORRADE_VERIFY(shaper->setLanguage("el"));
    CORRADE_VERIFY(shaper->setDirection(data.direction));
    CORRADE_COMPARE(shaper->shape("Ελλάδα"), 6);
    CORRADE_COMPARE(shaper->script(), Script::Greek);
    CORRADE_COMPARE(shaper->language(), "el");
    CORRADE_COMPARE(shaper->direction(), data.direction);

    /* HarfBuzz always shapes from left to right and top to bottom, so if the
       direction is opposite, the glyph IDs are in reverse direction */
    UnsignedInt ids[6];
    shaper->glyphIdsInto(ids);
    UnsignedInt expectedIds[]{
        450,    /* 'Ε' */
        487,    /* 'λ' */
        487,    /* 'λ' again */
        472,    /* 'ά' */
        480,    /* 'δ' */
        477,    /* 'α' */
    };
    CORRADE_COMPARE_AS(Containers::arrayView(ids),
        data.flip ?
            Containers::stridedArrayView(expectedIds).flipped<0>() :
            Containers::stridedArrayView(expectedIds),
        TestSuite::Compare::Container);

    /* Advances and offsets aren't really important here */

    /* All characters are two-byte. Due to the glyph IDs being reversed for
       opposite direction, the clusters get as well. */
    UnsignedInt clusters[6];
    shaper->glyphClustersInto(clusters);
    UnsignedInt expectedClusters[]{0, 2, 4, 6, 8, 10};
    CORRADE_COMPARE_AS(Containers::arrayView(clusters),
        data.flip ?
            Containers::stridedArrayView(expectedClusters).flipped<0>() :
            Containers::stridedArrayView(expectedClusters),
        TestSuite::Compare::Container);
}

void HarfBuzzFontTest::shapeAutodetectScriptLanguageDirection() {
    auto&& data = ShapeAutodetectScriptLanguageDirectionData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    if(data.explicitlySetUnspecified) {
        CORRADE_VERIFY(shaper->setScript(Script::Unspecified));
        CORRADE_VERIFY(shaper->setLanguage(""));
        CORRADE_VERIFY(shaper->setDirection(ShapeDirection::Unspecified));
    }

    CORRADE_COMPARE(shaper->shape("	العربية"), 8);
    CORRADE_COMPARE(shaper->script(), Script::Arabic);
    {
        CORRADE_EXPECT_FAIL("HarfBuzz uses current locale for language autodetection, not the actual text");
        CORRADE_COMPARE(shaper->language(), "ar");
    }
    CORRADE_COMPARE(shaper->language(), "c");
    CORRADE_COMPARE(shaper->direction(), ShapeDirection::RightToLeft);

    /* The font doesn't have Arabic glyphs, so this is all invalid */
    UnsignedInt ids[8];
    shaper->glyphIdsInto(ids);
    CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
    }), TestSuite::Compare::Container);
}

void HarfBuzzFontTest::shapeUnsupportedScript() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* Passing an unknown Script value will pass, as the check is
       blacklist-based to not have to switch through all possible values on
       HarfBuzz versions that support everything. Plus that also allows to
       pass values that new HarfBuzz supports but Script doesn't list yet, a
       whitelist would reject that. */
    CORRADE_VERIFY(shaper->setScript(script("Yolo")));

    /* Added in 3.0 */
    CORRADE_COMPARE(shaper->setScript(Script::OldUyghur),
                    HB_VERSION_ATLEAST(3, 0, 0));
    /* Added in 3.4 */
    CORRADE_COMPARE(shaper->setScript(Script::Math),
                    HB_VERSION_ATLEAST(3, 4, 0));
    /* Added in 5.2 */
    #if HB_VERSION_ATLEAST(5, 2, 0)
    CORRADE_SKIP("Can only test on HarfBuzz before 5.2.0");
    #endif
    CORRADE_VERIFY(!shaper->setScript(Script::Kawi));
}

void HarfBuzzFontTest::shapeEmpty() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* Shouldn't crash or do anything rogue */
    CORRADE_COMPARE(shaper->shape("Wave", 2, 2), 0);

    /* Interestingly enough it doesn't detect the script even though it has
       the surrounding context to guess from */
    CORRADE_COMPARE(shaper->script(), Script::Unspecified);
    CORRADE_COMPARE(shaper->language(), "c");
    CORRADE_COMPARE(shaper->direction(), ShapeDirection::LeftToRight);
}

void HarfBuzzFontTest::shapeGlyphOffset() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    /* See FreeTypeFont's test CMakeLists for details how this file was made.
       In particular, it has to include glyphs for
       FreeTypeFontTest::glyphNames() as well. */
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "SourceSans3-Regular.subset.otf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* "Ve͏̌̌̌tev", with the diacritics being three times above e, and U+034 in
       between (\xcd\x8f) to prevent them from being combined,
       https://stackoverflow.com/a/47979890). Should result in non-zero X
       offset and Y offset increasing for every subsequent diacritics mark.

       In this particular case the \xcd\x8f doesn't need to be present and it
       still works the same way, omitting the space character, but with just a
       single combining diacritics and a non-subset font that contains `ě` as
       well it could result in it being combined back to a single glyph. Then,
       `ccmp` / Feature::GlyphCompositionDecomposition turned off could force
       use of the combining glyphs again. */
    CORRADE_COMPARE(shaper->shape("Ve\xcd\x8f\xcc\x8c\xcc\x8c\xcc\x8ctev"), 9);

    UnsignedInt ids[9];
    Vector2 offsets[9];
    Vector2 advances[9];
    UnsignedInt clusters[9];
    shaper->glyphIdsInto(ids);
    shaper->glyphOffsetsAdvancesInto(offsets, advances);
    shaper->glyphClustersInto(clusters);
    CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
        font->glyphForName("V"),        /* glyph 23 originally */
        font->glyphForName("e"),        /* glyph 32 originally */
        font->glyphForName("space"),    /* glyph 1 originally */
        font->glyphForName("uni030C"),  /* glyph 2328 originally, 'ˇ' */
        font->glyphForName("uni030C"),  /* glyph 2328 originally, 'ˇ' */
        font->glyphForName("uni030C"),  /* glyph 2328 originally, 'ˇ' */
        font->glyphForName("t"),        /* glyph 47 originally */
        font->glyphForName("e"),        /* glyph 32 originally */
        font->glyphForName("v")         /* glyph 49 originally */
    }), TestSuite::Compare::Container);
    const Vector2 expectedAdvances[]{
        /* HarfBuzz 2.6.4 gives different output than 1.7.2 and 11 */
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR != 206
        {8.0f, 0.0f},           /* 'V' */
        /* HarfBuzz between 1.7 and 3.1 gives different output (broken? see
           offsets below). Actually, I have no idea if it's those versions, but
           version 11 (latest in Apr 2025) and version 1.7.2 (in Ubuntu 20.04
           repos) give a different value and this version check was in other
           places already so I'm betting it's related to the same
           difference. */
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 || \
            HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301
        {7.78125f, 0.0f},       /* 'e' */
        #else
        {7.9375f, 0.0f},        /* 'e' */
        #endif
        /* The combining marks have no advance in addition to the base
           character */
        {0.0f, 0.0f},           /* (space) */
        {0.0f, 0.0f},           /* 'ˇ' */
        {0.0f, 0.0f},           /* 'ˇ' */
        {0.0f, 0.0f},           /* 'ˇ' */
        {5.1875f, 0.0f},        /* 't' */
        #else
        {7.98438f, 0.0f},       /* 'V' */
        {7.76562f, 0.0f},       /* 'e' */
        {0.0f, 0.0f},           /* (space) */
        {0.0f, 0.0f},           /* 'ˇ' */
        {0.0f, 0.0f},           /* 'ˇ' */
        {0.0f, 0.0f},           /* 'ˇ' */
        {5.17188f, 0.0f},       /* 't' */
        #endif
        {8.01562f, 0.0f},       /* 'e' */
        {7.46875f, 0.0f}        /* 'v' */
    };
    const Vector2 expectedOffsets[]{
        {},                     /* 'V' */
        {},                     /* 'e' */
        {},                     /* (space) */
        /* HarfBuzz 2.6.4 gives different output than 1.7.2 and 11 */
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR != 206
        /* See above */
        #if HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 || \
            HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301
        /* Diacritics shifted to the left and then increasingly upwards */
        {-3.5625f, 0.0f},       /* 'ˇ' */
        {-3.5625f, 3.32812f},   /* 'ˇ' */
        {-3.5625f, 6.65625f},   /* 'ˇ' */
        #else
        /* Here it's without shift left, so ... likely broken?! */
        {0.0f, 0.0f},           /* 'ˇ' */
        {0.0f, 3.32812f},       /* 'ˇ' */
        {0.0f, 6.65625f},       /* 'ˇ' */
        #endif
        #else
        {-3.54688f, 0.0f},      /* 'ˇ' */
        {-3.54688f, 3.32812f},  /* 'ˇ' */
        {-3.54688f, 6.65625f},  /* 'ˇ' */
        #endif
        {},                     /* 't' */
        {},                     /* 'e' */
        {}                      /* 'v' */
    };
    CORRADE_COMPARE_AS(Containers::arrayView(advances),
        Containers::arrayView(expectedAdvances),
        TestSuite::Compare::Container);
    CORRADE_COMPARE_AS(Containers::arrayView(offsets),
        Containers::arrayView(expectedOffsets),
        TestSuite::Compare::Container);
    /** @todo with `hb_buffer_set_cluster_level(_buffer,
        HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES)` this would result in
        `{0, 1, 2, 4, 6, 8, 10, 11, 12}` instead, expose that in some way? */
    CORRADE_COMPARE_AS(Containers::arrayView(clusters), Containers::arrayView({
        0u,                     /* 'V' */
        /* The base character together with all combining marks is a single
           cluster */
        1u,                     /* 'e' */
        1u,                     /* (space) */
        1u,                     /* 'ˇ' */
        1u,                     /* 'ˇ' */
        1u,                     /* 'ˇ' */
        10u,                    /* 't' */
        11u,                    /* 'e' */
        12u,                    /* 'v' */
    }), TestSuite::Compare::Container);
}

void HarfBuzzFontTest::shapeMultiple() {
    auto&& data = ShapeMultipleData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* Empty text */
    {
        CORRADE_COMPARE(shaper->shape("Wave", 2, 2), 0);

    /* Short text. Empty shape shouldn't have caused any broken state. */
    } {
        if(!data.reuse)
            shaper = font->createShaper();

        CORRADE_COMPARE(shaper->shape("We"), 2);
        UnsignedInt ids[2];
        Vector2 offsets[2];
        Vector2 advances[2];
        UnsignedInt clusters[2];
        shaper->glyphIdsInto(ids);
        shaper->glyphOffsetsAdvancesInto(offsets, advances);
        shaper->glyphClustersInto(clusters);
        CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
            58u, /* 'W' */
            72u  /* 'e' */
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(offsets), Containers::arrayView<Vector2>({
            {}, {},
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(advances), Containers::arrayView<Vector2>({
            {HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 ||
             HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301 ? 16.3125f : 16.2969f,
             0.0f},
            {8.34375f, 0.0f}
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(clusters), Containers::arrayView({
            0u,
            1u
        }), TestSuite::Compare::Container);

    /* Long text, same as in shape(), should enlarge the array for it */
    } {
        if(!data.reuse)
            shaper = font->createShaper();

        CORRADE_COMPARE(shaper->shape("Wěave"), 5);
        UnsignedInt ids[5];
        Vector2 offsets[5];
        Vector2 advances[5];
        UnsignedInt clusters[5];
        shaper->glyphIdsInto(ids);
        shaper->glyphOffsetsAdvancesInto(offsets, advances);
        shaper->glyphClustersInto(clusters);
        CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
            58u,  /* 'W' */
            220u, /* 'ě' */
            68u,  /* 'a' */
            89u,  /* 'v' */
            72u   /* 'e' */
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(offsets), Containers::arrayView<Vector2>({
            {}, {}, {}, {}, {}
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(advances), Containers::arrayView<Vector2>({
            {16.6562f, 0.0f},
            {8.34375f, 0.0f},
            {8.26562f, 0.0f},
            {HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 ||
             HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301 ? 8.0f : 7.984384f,
             0.0f},
            {8.34375f, 0.0f}
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(clusters), Containers::arrayView({
            0u,
            1u,
            3u,
            4u,
            5u
        }), TestSuite::Compare::Container);

    /* Short text again, should not leave the extra glyphs there */
    } {
        if(!data.reuse)
            shaper = font->createShaper();

        CORRADE_COMPARE(shaper->shape("ave"), 3);
        UnsignedInt ids[3];
        Vector2 offsets[3];
        Vector2 advances[3];
        UnsignedInt clusters[3];
        shaper->glyphIdsInto(ids);
        shaper->glyphOffsetsAdvancesInto(offsets, advances);
        shaper->glyphClustersInto(clusters);
        CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
            68u, 89u, 72u
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(offsets), Containers::arrayView<Vector2>({
            {}, {}, {}
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(advances), Containers::arrayView<Vector2>({
            {8.26562f, 0.0f},
            {HB_VERSION_MAJOR*100 + HB_VERSION_MINOR < 107 ||
             HB_VERSION_MAJOR*100 + HB_VERSION_MINOR >= 301 ? 8.0f : 7.984384f,
             0.0f},
            {8.34375f, 0.0f}
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(clusters), Containers::arrayView({
            0u, 1u, 2u
        }), TestSuite::Compare::Container);
    }
}

void HarfBuzzFontTest::shapeMultipleAutodetection() {
    auto&& data = ShapeMultipleData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* There's no script / language / direction set by default */
    CORRADE_COMPARE(shaper->script(), Script::Unspecified);
    CORRADE_COMPARE(shaper->language(), "");
    CORRADE_COMPARE(shaper->direction(), ShapeDirection::Unspecified);

    /* Arabic text gets detected as such */
    {
        if(!data.reuse)
            shaper = font->createShaper();

        CORRADE_COMPARE(shaper->shape("	العربية"), 8);
        CORRADE_COMPARE(shaper->script(), Script::Arabic);
        {
            CORRADE_EXPECT_FAIL("HarfBuzz uses current locale for language autodetection, not the actual text");
            CORRADE_COMPARE(shaper->language(), "ar");
        }
        CORRADE_COMPARE(shaper->language(), "c");
        CORRADE_COMPARE(shaper->direction(), ShapeDirection::RightToLeft);

    /* Greek text should then not be treated as RTL and such */
    } {
        if(!data.reuse)
            shaper = font->createShaper();

        CORRADE_COMPARE(shaper->shape("Ελλάδα"), 6);
        CORRADE_COMPARE(shaper->script(), Script::Greek);
        {
            CORRADE_EXPECT_FAIL("HarfBuzz uses current locale for language autodetection, not the actual text");
            CORRADE_COMPARE(shaper->language(), "el");
        }
        CORRADE_COMPARE(shaper->language(), "c");
        CORRADE_COMPARE(shaper->direction(), ShapeDirection::LeftToRight);

    /* Empty text shouldn't inherit anything from before either and produce a
       result consistent with shapeEmpty() */
    } {
        if(!data.reuse)
            shaper = font->createShaper();

        CORRADE_COMPARE(shaper->shape("Wave", 2, 2), 0);
        CORRADE_COMPARE(shaper->script(), Script::Unspecified);
        CORRADE_COMPARE(shaper->language(), "c");
        CORRADE_COMPARE(shaper->direction(), ShapeDirection::LeftToRight);
    }
}

void HarfBuzzFontTest::shapeFeatures() {
    auto&& data = ShapeFeaturesData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* Shape a text */
    CORRADE_VERIFY(shaper->setScript(Script::Latin));
    CORRADE_VERIFY(shaper->setLanguage("en"));
    CORRADE_VERIFY(shaper->setDirection(ShapeDirection::LeftToRight));
    CORRADE_COMPARE(shaper->shape("Wave", data.features), 4);

    /* Verify the shaped glyph IDs match expectations, other IDs would have
       vastly different advances */
    UnsignedInt ids[4];
    Vector2 offsets[4];
    Vector2 advances[4];
    shaper->glyphIdsInto(ids);
    shaper->glyphOffsetsAdvancesInto(offsets, advances);
    CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
        58u,    /* 'W' */
        68u,    /* 'a' */
        89u,    /* 'v' */
        72u,    /* 'e' */
    }), TestSuite::Compare::Container);

    /* Assuming Y advance is always 0 */
    CORRADE_COMPARE_AS(Containers::stridedArrayView(advances).slice(&Vector2::x),
        Containers::stridedArrayView(data.advances),
        TestSuite::Compare::Container);
}

void HarfBuzzFontTest::openTwice() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("HarfBuzzFont");

    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    /* Shouldn't crash, leak or anything */
}

}}}}

CORRADE_TEST_MAIN(Magnum::Text::Test::HarfBuzzFontTest)
