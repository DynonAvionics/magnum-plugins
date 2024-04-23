/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022, 2023 Vladimír Vondruš <mosra@centrum.cz>

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

#include <sstream>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/StridedArrayView.h>
#include <Corrade/Containers/String.h>
#include <Corrade/Containers/Triple.h>
#include <Corrade/PluginManager/Manager.h>
#include <Corrade/TestSuite/Tester.h>
#include <Corrade/TestSuite/Compare/Container.h>
#include <Corrade/Utility/DebugStl.h> /** @todo remove once Debug is stream-free */
#include <Corrade/Utility/Path.h>
#include <Magnum/ImageView.h>
#include <Magnum/Math/Range.h>
#include <Magnum/DebugTools/CompareImage.h>
#include <Magnum/Text/AbstractFont.h>
#include <Magnum/Text/AbstractGlyphCache.h>
#include <Magnum/Text/AbstractShaper.h>
#include <Magnum/Trade/AbstractImporter.h>

#include "configure.h"

namespace Magnum { namespace Text { namespace Test { namespace {

struct FreeTypeFontTest: TestSuite::Tester {
    explicit FreeTypeFontTest();

    void empty();
    void invalid();

    void properties();

    void shape();
    void shapeEmpty();
    void shaperReuse();

    void fillGlyphCache();
    void fillGlyphCacheIncremental();
    void fillGlyphCacheArray();
    void fillGlyphCacheInvalidFormat();
    void fillGlyphCacheCannotFit();

    /* Explicitly forbid system-wide plugin dependencies */
    PluginManager::Manager<AbstractFont> _manager{"nonexistent"};

    /* Needs to load AnyImageImporter from system-wide location */
    PluginManager::Manager<Trade::AbstractImporter> _importerManager;
};

const struct {
    const char* name;
    const char* string;
    UnsignedInt eGlyphId;
    UnsignedInt begin, end;
} ShapeData[]{
    {"", "Wave", 72, 0, ~UnsignedInt{}},
    {"substring", "haWavefefe", 72, 2, 6},
    {"UTF-8", "Wavě", 220, 0, ~UnsignedInt{}},
    {"UTF-8 substring", "haWavěfefe", 220, 2, 7},
};

const struct {
    const char* name;
    const char* characters;
} FillGlyphCacheData[]{
    {"",
        /* Including also UTF-8 characters to be sure they're handled
           properly */
        "abcdefghijklmnopqrstuvwxyzěšč"},
    {"shuffled order",
        "mvxěipbryzdhfnqlčjšswutokeacg"},
    {"duplicates",
        "mvexěipbbrzzyčbjzdgšhhfnqljswutokeakcg"},
    {"characters not in font",
        /* ☃ */
        "abcdefghijkl\xe2\x98\x83mnopqrstuvwxyzěšč"},
};

FreeTypeFontTest::FreeTypeFontTest() {
    addTests({&FreeTypeFontTest::empty,
              &FreeTypeFontTest::invalid,

              &FreeTypeFontTest::properties});

    addInstancedTests({&FreeTypeFontTest::shape},
        Containers::arraySize(ShapeData));

    addTests({&FreeTypeFontTest::shapeEmpty,
              &FreeTypeFontTest::shaperReuse});

    addInstancedTests({&FreeTypeFontTest::fillGlyphCache},
        Containers::arraySize(FillGlyphCacheData));

    addTests({&FreeTypeFontTest::fillGlyphCacheIncremental,
              &FreeTypeFontTest::fillGlyphCacheArray,
              &FreeTypeFontTest::fillGlyphCacheInvalidFormat,
              &FreeTypeFontTest::fillGlyphCacheCannotFit});

    /* Load the plugin directly from the build tree. Otherwise it's static and
       already loaded. */
    #ifdef FREETYPEFONT_PLUGIN_FILENAME
    CORRADE_INTERNAL_ASSERT_OUTPUT(_manager.load(FREETYPEFONT_PLUGIN_FILENAME) & PluginManager::LoadState::Loaded);
    #endif
    /* Pull in the AnyImageImporter dependency for image comparison */
    _importerManager.load("AnyImageImporter");
    /* Reset the plugin dir after so it doesn't load anything else from the
       filesystem. Do this also in case of static plugins (no _FILENAME
       defined) so it doesn't attempt to load dynamic system-wide plugins. */
    #ifndef CORRADE_PLUGINMANAGER_NO_DYNAMIC_PLUGIN_SUPPORT
    _importerManager.setPluginDirectory({});
    #endif
    /* Load StbImageImporter from the build tree, if defined. Otherwise it's
       static and already loaded. */
    #ifdef STBIMAGEIMPORTER_PLUGIN_FILENAME
    CORRADE_INTERNAL_ASSERT_OUTPUT(_importerManager.load(STBIMAGEIMPORTER_PLUGIN_FILENAME) & PluginManager::LoadState::Loaded);
    #endif
}

void FreeTypeFontTest::empty() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");

    std::ostringstream out;
    Error redirectError{&out};
    char a{};
    /* Explicitly checking non-null but empty view */
    CORRADE_VERIFY(!font->openData({&a, 0}, 16.0f));
    CORRADE_COMPARE(out.str(), "Text::FreeTypeFont::openData(): failed to open the font: 6\n");
}

void FreeTypeFontTest::invalid() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");

    std::ostringstream out;
    Error redirectError{&out};
    CORRADE_VERIFY(!font->openData("Oxygen.ttf", 16.0f));
    CORRADE_COMPARE(out.str(), "Text::FreeTypeFont::openData(): failed to open the font: 85\n");
}

void FreeTypeFontTest::properties() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));
    CORRADE_COMPARE(font->size(), 16.0f);
    CORRADE_COMPARE(font->ascent(), 15.0f);
    CORRADE_COMPARE(font->descent(), -4.0f);
    CORRADE_COMPARE(font->lineHeight(), 19.0f);
    CORRADE_COMPARE(font->glyphCount(), 671);
    CORRADE_COMPARE(font->glyphId(U'W'), 58);
    CORRADE_COMPARE(font->glyphSize(58), Vector2(18.0f, 12.0f));
    CORRADE_COMPARE(font->glyphAdvance(58), Vector2(17.0f, 0.0f));
}

void FreeTypeFontTest::shape() {
    auto&& data = ShapeData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    CORRADE_COMPARE(shaper->shape(data.string, data.begin, data.end), 4);

    UnsignedInt ids[4];
    Vector2 offsets[4];
    Vector2 advances[4];
    shaper->glyphIdsInto(ids);
    shaper->glyphOffsetsAdvancesInto(offsets, advances);
    CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
        58u,            /* 'W' */
        68u,            /* 'a' */
        89u,            /* 'v' */
        data.eGlyphId   /* 'e' or 'ě' */
    }), TestSuite::Compare::Container);
    /* There are no glyph-specific offsets here */
    CORRADE_COMPARE_AS(Containers::arrayView(offsets), Containers::arrayView<Vector2>({
        {}, {}, {}, {}
    }), TestSuite::Compare::Container);
    CORRADE_COMPARE_AS(Containers::arrayView(advances), Containers::arrayView<Vector2>({
        {17.0f, 0.0f},
        {8.0f, 0.0f},
        {8.0f, 0.0f},
        {9.0f, 0.0f}
    }), TestSuite::Compare::Container);
}

void FreeTypeFontTest::shapeEmpty() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* Shouldn't crash or do anything rogue */
    CORRADE_COMPARE(shaper->shape("Wave", 2, 2), 0);
}

void FreeTypeFontTest::shaperReuse() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    Containers::Pointer<AbstractShaper> shaper = font->createShaper();

    /* Empty text */
    {
        CORRADE_COMPARE(shaper->shape("Wave", 2, 2), 0);

    /* Short text. Empty shape shouldn't have caused any broken state. */
    } {
        CORRADE_COMPARE(shaper->shape("We"), 2);
        UnsignedInt ids[2];
        Vector2 offsets[2];
        Vector2 advances[2];
        shaper->glyphIdsInto(ids);
        shaper->glyphOffsetsAdvancesInto(offsets, advances);
        CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
            58u, /* 'W' */
            72u  /* 'e' */
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(offsets), Containers::arrayView<Vector2>({
            {}, {},
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(advances), Containers::arrayView<Vector2>({
            {17.0f, 0.0f},
            {9.0f, 0.0f}
        }), TestSuite::Compare::Container);

    /* Long text, same as in shape(), should enlarge the array for it */
    } {
        CORRADE_COMPARE(shaper->shape("Wave"), 4);
        UnsignedInt ids[4];
        Vector2 offsets[4];
        Vector2 advances[4];
        shaper->glyphIdsInto(ids);
        shaper->glyphOffsetsAdvancesInto(offsets, advances);
        CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
            58u, /* 'W' */
            68u, /* 'a' */
            89u, /* 'v' */
            72u  /* 'e' or 'ě' */
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(offsets), Containers::arrayView<Vector2>({
            {}, {}, {}, {}
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(advances), Containers::arrayView<Vector2>({
            {17.0f, 0.0f},
            {8.0f, 0.0f},
            {8.0f, 0.0f},
            {9.0f, 0.0f}
        }), TestSuite::Compare::Container);

    /* Short text again, should not leave the extra glyphs there */
    } {
        CORRADE_COMPARE(shaper->shape("a"), 1);
        UnsignedInt ids[1];
        Vector2 offsets[1];
        Vector2 advances[1];
        shaper->glyphIdsInto(ids);
        shaper->glyphOffsetsAdvancesInto(offsets, advances);
        CORRADE_COMPARE_AS(Containers::arrayView(ids), Containers::arrayView({
            68u,
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(offsets), Containers::arrayView<Vector2>({
            {},
        }), TestSuite::Compare::Container);
        CORRADE_COMPARE_AS(Containers::arrayView(advances), Containers::arrayView<Vector2>({
            {8.0f, 0.0f}
        }), TestSuite::Compare::Container);
    }
}

void FreeTypeFontTest::fillGlyphCache() {
    auto&& data = FillGlyphCacheData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    /* Ideally this would be tested at least partially without the image, but
       adding extra logic for that would risk that the image might accidentally
       not get checked at all */
    if(_importerManager.loadState("PngImporter") == PluginManager::LoadState::NotFound)
        CORRADE_SKIP("PngImporter plugin not found, cannot test");

    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    struct GlyphCache: AbstractGlyphCache {
        explicit GlyphCache(PluginManager::Manager<Trade::AbstractImporter>& importerManager, PixelFormat format, const Vector2i& size, const Vector2i& padding): AbstractGlyphCache{format, size, padding}, importerManager(importerManager) {}

        GlyphCacheFeatures doFeatures() const override { return {}; }
        void doSetImage(const Vector2i& offset, const ImageView2D& image) override {
            /* The passed image is just the filled subset, compare the whole
               thing for more predictable results */
            CORRADE_COMPARE(offset, Vector2i{});
            CORRADE_COMPARE(image.size(), (Vector2i{64, 46}));
            CORRADE_COMPARE_WITH(this->image().pixels<UnsignedByte>()[0],
                Utility::Path::join(FREETYPEFONT_TEST_DIR, "glyph-cache.png"),
                DebugTools::CompareImageToFile{importerManager});
            called = true;
        }

        bool called = false;
        PluginManager::Manager<Trade::AbstractImporter>& importerManager;
    /* Default padding is 1, set back to 0 to verify it's not overwriting
       neighbors by accident */
    } cache{_importerManager, PixelFormat::R8Unorm, Vector2i{64}, {}};

    /* Should call doSetImage() above, which then performs image comparison */
    font->fillGlyphCache(cache, data.characters);
    CORRADE_VERIFY(cache.called);

    /* The font should associate itself with the cache */
    CORRADE_COMPARE(cache.fontCount(), 1);
    CORRADE_COMPARE(cache.findFont(*font), 0);

    /* 26 ASCII characters, 3 UTF-8 ones + one "not found" glyph, and one
       invalid glyph from the cache itself. The count should be the same in all
       cases as the input is deduplicated and characters not present in the
       font get substituted for glyph 0. */
    CORRADE_COMPARE(cache.glyphCount(), 26 + 3 + 1 + 1);

    /* Check positions of a few select glyphs. They should all retain the same
       position regardless of how the input is shuffled. */

    /* Invalid glyph in the cache is deliberately not changed as that'd cause
       a mess if multiple fonts would each want to set its own */
    CORRADE_COMPARE(cache.glyph(0), Containers::triple(
        Vector2i{},
        0,
        Range2Di{}));
    /* Invalid glyph */
    CORRADE_COMPARE(cache.glyph(0, 0), Containers::triple(
        Vector2i{},
        0,
        Range2Di{{59, 26}, {64, 37}}));
    /* Above the baseline */
    CORRADE_COMPARE(cache.glyph(0, font->glyphId('k')), Containers::triple(
        Vector2i{},
        0,
        Range2Di{{29, 14}, {37, 27}}));
    /* Below the baseline */
    CORRADE_COMPARE(cache.glyph(0, font->glyphId('g')), Containers::triple(
        Vector2i{0, -4},
        0,
        Range2Di{{48, 0}, {57, 13}}));
    /* UTF-8 */
    UnsignedInt sId = font->glyphId(
        /* MSVC (but not clang-cl) doesn't support UTF-8 in char32_t literals
           but it does it regular strings. Still a problem in MSVC 2022, what a
           trash fire, can't you just give up on those codepage insanities
           already, ffs?! */
        #if defined(CORRADE_TARGET_MSVC) && !defined(CORRADE_TARGET_CLANG)
        U'\u0161'
        #else
        U'š'
        #endif
    );
    CORRADE_COMPARE(cache.glyph(0, sId), Containers::triple(
        Vector2i{},
        0,
        Range2Di{{22, 0}, {30, 14}}));
}

void FreeTypeFontTest::fillGlyphCacheIncremental() {
    /* Ideally this would be tested at least partially without the image, but
       adding extra logic for that would risk that the image might accidentally
       not get checked at all */
    if(_importerManager.loadState("PngImporter") == PluginManager::LoadState::NotFound)
        CORRADE_SKIP("PngImporter plugin not found, cannot test");

    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    struct GlyphCache: AbstractGlyphCache {
        explicit GlyphCache(PluginManager::Manager<Trade::AbstractImporter>& importerManager, PixelFormat format, const Vector2i& size, const Vector2i& padding): AbstractGlyphCache{format, size, padding}, importerManager(importerManager) {}

        GlyphCacheFeatures doFeatures() const override { return {}; }
        void doSetImage(const Vector2i& offset, const ImageView2D& image) override {
            /* The passed image is just the filled subset, compare the whole
               thing for more predictable results */
            if(called == 0) {
                CORRADE_COMPARE(offset, Vector2i{});
                CORRADE_COMPARE(image.size(), (Vector2i{64, 37}));
            } else if(called == 1) {
                CORRADE_COMPARE(offset, (Vector2i{0, 26}));
                CORRADE_COMPARE(image.size(), (Vector2i{61, 20}));
                CORRADE_COMPARE_WITH(this->image().pixels<UnsignedByte>()[0],
                    Utility::Path::join(FREETYPEFONT_TEST_DIR, "glyph-cache.png"),
                    DebugTools::CompareImageToFile{importerManager});
            } else CORRADE_FAIL("This shouldn't get called more than twice");
            ++called;
        }

        Int called = 0;
        PluginManager::Manager<Trade::AbstractImporter>& importerManager;
    /* Default padding is 1, set back to 0 to verify it's not overwriting
       neighbors by accident */
    } cache{_importerManager, PixelFormat::R8Unorm, Vector2i{64}, {}};

    /* First call with the bottom half of the glyph cache until the invalid
       glyph */
    font->fillGlyphCache(cache, "jěčšbdghpqkylfti");
    CORRADE_COMPARE(cache.called, 1);

    /* The font should associate itself with the cache now */
    CORRADE_COMPARE(cache.fontCount(), 1);
    CORRADE_COMPARE(cache.findFont(*font), 0);

    /* 17 characters + one global invalid glyph */
    CORRADE_COMPARE(cache.glyphCount(), 17 + 1);

    /* Second call with the rest */
    font->fillGlyphCache(cache, "mwovenuacsxzr");
    CORRADE_COMPARE(cache.called, 2);

    /* The font should not be added again */
    CORRADE_COMPARE(cache.fontCount(), 1);

    /* There's now all glyphs like in fillGlyphCache() */
    CORRADE_COMPARE(cache.glyphCount(), 26 + 3 + 1 + 1);

    /* Positions of the glyphs should be the same as in fillGlyphCache() */
    CORRADE_COMPARE(cache.glyph(0), Containers::triple(
        Vector2i{},
        0,
        Range2Di{}));
    CORRADE_COMPARE(cache.glyph(0, 0), Containers::triple(
        Vector2i{},
        0,
        Range2Di{{59, 26}, {64, 37}}));
    CORRADE_COMPARE(cache.glyph(0, font->glyphId('k')), Containers::triple(
        Vector2i{},
        0,
        Range2Di{{29, 14}, {37, 27}}));
    CORRADE_COMPARE(cache.glyph(0, font->glyphId('g')), Containers::triple(
        Vector2i{0, -4},
        0,
        Range2Di{{48, 0}, {57, 13}}));
    UnsignedInt sId = font->glyphId(
        /* MSVC (but not clang-cl) doesn't support UTF-8 in char32_t literals
           but it does it regular strings. Still a problem in MSVC 2022, what a
           trash fire, can't you just give up on those codepage insanities
           already, ffs?! */
        #if defined(CORRADE_TARGET_MSVC) && !defined(CORRADE_TARGET_CLANG)
        U'\u0161'
        #else
        U'š'
        #endif
    );
    CORRADE_COMPARE(cache.glyph(0, sId), Containers::triple(
        Vector2i{},
        0,
        Range2Di{{22, 0}, {30, 14}}));
}

void FreeTypeFontTest::fillGlyphCacheArray() {
    /* Ideally this would be tested at least partially without the image, but
       adding extra logic for that would risk that the image might accidentally
       not get checked at all */
    if(_importerManager.loadState("PngImporter") == PluginManager::LoadState::NotFound)
        CORRADE_SKIP("PngImporter plugin not found, cannot test");

    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    struct GlyphCache: AbstractGlyphCache {
        explicit GlyphCache(PluginManager::Manager<Trade::AbstractImporter>& importerManager, PixelFormat format, const Vector3i& size, const Vector2i& padding): AbstractGlyphCache{format, size, padding}, importerManager(importerManager) {}

        GlyphCacheFeatures doFeatures() const override { return {}; }
        void doSetImage(const Vector3i& offset, const ImageView3D& image) override {
            /* The passed image is just the filled subset, compare the whole
               thing for more predictable results */
            CORRADE_COMPARE(offset, Vector3i{});
            CORRADE_COMPARE(image.size(), (Vector3i{48, 48, 2}));
            CORRADE_COMPARE_WITH(this->image().pixels<UnsignedByte>()[0],
                Utility::Path::join(FREETYPEFONT_TEST_DIR, "glyph-cache-array0.png"),
                DebugTools::CompareImageToFile{importerManager});
            CORRADE_COMPARE_WITH(this->image().pixels<UnsignedByte>()[1],
                Utility::Path::join(FREETYPEFONT_TEST_DIR, "glyph-cache-array1.png"),
                DebugTools::CompareImageToFile{importerManager});
            called = true;
        }

        bool called = false;
        PluginManager::Manager<Trade::AbstractImporter>& importerManager;
    /* Default padding is 1, set back to 0 to verify it's not overwriting
       neighbors by accident */
    } cache{_importerManager, PixelFormat::R8Unorm, {48, 48, 2}, {}};

    /* Should call doSetImage() above, which then performs image comparison */
    font->fillGlyphCache(cache, "abcdefghijklmnopqrstuvwxyzěšč");
    CORRADE_VERIFY(cache.called);

    /* The font should associate itself with the cache */
    CORRADE_COMPARE(cache.fontCount(), 1);
    CORRADE_COMPARE(cache.findFont(*font), 0);

    /* Same as in fillGlyphCache() */
    CORRADE_COMPARE(cache.glyphCount(), 26 + 3 + 1 + 1);

    /* Positions are spread across two layers now */
    CORRADE_COMPARE(cache.glyph(0), Containers::triple(
        Vector2i{},
        0,
        Range2Di{}));
    /* Invalid glyph */
    CORRADE_COMPARE(cache.glyph(0, 0), Containers::triple(
        Vector2i{},
        0,
        Range2Di{{15, 27}, {20, 38}}));
    /* First layer */
    CORRADE_COMPARE(cache.glyph(0, font->glyphId('g')), Containers::triple(
        Vector2i{0, -4},
        0,
        Range2Di{{39, 13}, {48, 26}}));
    /* Second layer */
    CORRADE_COMPARE(cache.glyph(0, font->glyphId('n')), Containers::triple(
        Vector2i{0, 0},
        1,
        Range2Di{{0, 0}, {9, 9}}));
}

void FreeTypeFontTest::fillGlyphCacheInvalidFormat() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    struct GlyphCache: AbstractGlyphCache {
        explicit GlyphCache(PixelFormat format, const Vector2i& size, const Vector2i& padding): AbstractGlyphCache{format, size, padding} {}

        GlyphCacheFeatures doFeatures() const override { return {}; }
        void doSetImage(const Vector3i&, const ImageView3D&) override {
            CORRADE_FAIL("This shouldn't be called.");
        }
    } cache{PixelFormat::R8Srgb, {16, 16}, {}};

    std::ostringstream out;
    Error redirectError{&out};
    font->fillGlyphCache(cache, "");
    CORRADE_COMPARE(out.str(), "Text::FreeTypeFont::fillGlyphCache(): expected a PixelFormat::R8Unorm glyph cache but got PixelFormat::R8Srgb\n");
}

void FreeTypeFontTest::fillGlyphCacheCannotFit() {
    Containers::Pointer<AbstractFont> font = _manager.instantiate("FreeTypeFont");
    CORRADE_VERIFY(font->openFile(Utility::Path::join(FREETYPEFONT_TEST_DIR, "Oxygen.ttf"), 16.0f));

    struct GlyphCache: AbstractGlyphCache {
        explicit GlyphCache(PixelFormat format, const Vector2i& size, const Vector2i& padding): AbstractGlyphCache{format, size, padding} {}

        GlyphCacheFeatures doFeatures() const override { return {}; }
        void doSetImage(const Vector3i&, const ImageView3D&) override {
            CORRADE_FAIL("This shouldn't be called.");
        }
    } cache{PixelFormat::R8Unorm, {16, 32}, {}};

    std::ostringstream out;
    Error redirectError{&out};
    font->fillGlyphCache(cache, "HELLO");
    CORRADE_COMPARE(out.str(), "Text::FreeTypeFont::fillGlyphCache(): cannot fit 5 glyphs with a total area of 535 pixels into a cache of size Vector(16, 32, 1) and Vector(16, 0, 1) filled so far\n");
}

}}}}

CORRADE_TEST_MAIN(Magnum::Text::Test::FreeTypeFontTest)
