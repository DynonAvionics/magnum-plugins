/*
    This file is part of Magnum.

    Copyright © 2010, 2011, 2012, 2013, 2014, 2015, 2016, 2017, 2018, 2019,
                2020, 2021, 2022, 2023, 2024, 2025
              Vladimír Vondruš <mosra@centrum.cz>
    Copyright © 2021 Pablo Escobar <mail@rvrs.in>

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

#include <algorithm> /* std::find() */
#include <unordered_map>
#include <Corrade/Containers/Array.h>
#include <Corrade/Containers/Optional.h>
#include <Corrade/Containers/Pair.h>
#include <Corrade/PluginManager/PluginMetadata.h>
#include <Corrade/TestSuite/Tester.h>
#include <Corrade/TestSuite/Compare/Container.h>
#include <Corrade/TestSuite/Compare/String.h>
#include <Corrade/TestSuite/Compare/StringToFile.h>
#include <Corrade/Utility/ConfigurationGroup.h>
#include <Corrade/Utility/DebugStl.h> /** @todo remove once Configuration is std::string-free */
#include <Corrade/Utility/Endianness.h>
#include <Corrade/Utility/Format.h>
#include <Corrade/Utility/Path.h>

#include <Magnum/ImageView.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/DebugTools/CompareImage.h>
#include <Magnum/Math/Color.h>
#include <Magnum/Trade/ImageData.h>
#include <Magnum/Trade/AbstractImageConverter.h>
#include <Magnum/Trade/AbstractImporter.h>
#include <Magnum/Trade/ImageData.h>

#include "MagnumPlugins/KtxImporter/KtxHeader.h"

#include "configure.h"

namespace Magnum { namespace Trade { namespace Test { namespace {

struct KtxImageConverterTest: TestSuite::Tester {
    explicit KtxImageConverterTest();

    void supportedFormat();
    void supportedCompressedFormat();
    void unsupportedFormat();
    void unsupportedCompressedFormat();
    void implementationSpecificFormat();
    void implementationSpecificCompressedFormat();

    void dataFormatDescriptor();
    void dataFormatDescriptorCompressed();

    /* Non-default compressed pixel storage is currently not supported.
       It's firing an internal assert, so we're not testing that. */
    void pixelStorage();

    void tooManyLevels();
    void levelWrongSize();

    void convert1D();
    void convert1DMipmaps();
    void convert1DCompressed();
    void convert1DCompressedMipmaps();

    void convert1DArray();
    /* Plenty of other combinations, no need to test 1D array with mips */

    void convert2D();
    void convert2DMipmaps();
    /* Should be enough to only test this for one type */
    void convert2DMipmapsIncomplete();
    void convert2DCompressed();
    void convert2DCompressedMipmaps();

    void convert2DArray();
    void convert2DArrayMipmaps();

    void convertCubeMap();
    void convertCubeMapMipmaps();
    void convertCubeMapArray();
    /* Plenty of other combinations, no need to test cube array with mips */

    void convert3D();
    void convert3DMipmaps();
    void convert3DCompressed();
    void convert3DCompressedMipmaps();

    void convertFormats();

    void pvrtcRgb();

    void configurationOrientation();
    void configurationOrientationLessDimensions();
    void configurationOrientationEmpty();
    void configurationOrientationInvalid();
    void configurationSwizzle();
    void configurationSwizzleEmpty();
    void configurationSwizzleInvalid();
    void configurationGenerator();
    void configurationGeneratorVersion();
    void configurationGeneratorEmpty();

    void configurationEmpty();
    void configurationSorted();

    void convertTwice();

    /* Explicitly forbid system-wide plugin dependencies */
    PluginManager::Manager<AbstractImageConverter> _converterManager{"nonexistent"};
    PluginManager::Manager<AbstractImporter> _importerManager{"nonexistent"};

    Containers::Array<char> dfdData;
    std::unordered_map<Implementation::VkFormat, Containers::ArrayView<const char>> dfdMap;
    /* Original generator name from config before it gets modified for
       predictable output files */
    Containers::String _originalGeneratorName;
};

using namespace Containers::Literals;
using namespace Math::Literals;

const Color3ub PatternRgb1DData[3][4]{
    /* pattern-1d.png */
    {0xff0000_rgb, 0xffffff_rgb, 0x000000_rgb, 0x007f7f_rgb},
    /* pattern-1d.png */
    {0xff0000_rgb, 0xffffff_rgb, 0x000000_rgb, 0x007f7f_rgb},
    /* black-1d.png */
    {0x000000_rgb, 0x000000_rgb, 0x000000_rgb, 0x000000_rgb}
};

/* Origin top-left-back (for a 3D texture) */
const Color3ub PatternRgbData[3][3][4]{
    /* black.png */
    {{0x000000_rgb, 0x000000_rgb, 0x000000_rgb, 0x000000_rgb},
     {0x000000_rgb, 0x000000_rgb, 0x000000_rgb, 0x000000_rgb},
     {0x000000_rgb, 0x000000_rgb, 0x000000_rgb, 0x000000_rgb}},
    /* pattern.png */
    {{0x0000ff_rgb, 0x00ff00_rgb, 0x7f007f_rgb, 0x7f007f_rgb},
     {0xffffff_rgb, 0xff0000_rgb, 0x000000_rgb, 0x00ff00_rgb},
     {0xff0000_rgb, 0xffffff_rgb, 0x000000_rgb, 0x00ff00_rgb}},
    /* pattern.png */
    {{0x0000ff_rgb, 0x00ff00_rgb, 0x7f007f_rgb, 0x7f007f_rgb},
     {0xffffff_rgb, 0xff0000_rgb, 0x000000_rgb, 0x00ff00_rgb},
     {0xff0000_rgb, 0xffffff_rgb, 0x000000_rgb, 0x00ff00_rgb}}
};

/* Origin top-left-forward (for a 2D array texture) */
const Color3ub PatternRgbData2DArray[3][3][4]{
    /* pattern.png */
    {{0x0000ff_rgb, 0x00ff00_rgb, 0x7f007f_rgb, 0x7f007f_rgb},
     {0xffffff_rgb, 0xff0000_rgb, 0x000000_rgb, 0x00ff00_rgb},
     {0xff0000_rgb, 0xffffff_rgb, 0x000000_rgb, 0x00ff00_rgb}},
    /* pattern.png */
    {{0x0000ff_rgb, 0x00ff00_rgb, 0x7f007f_rgb, 0x7f007f_rgb},
     {0xffffff_rgb, 0xff0000_rgb, 0x000000_rgb, 0x00ff00_rgb},
     {0xff0000_rgb, 0xffffff_rgb, 0x000000_rgb, 0x00ff00_rgb}},
    /* black.png */
    {{0x000000_rgb, 0x000000_rgb, 0x000000_rgb, 0x000000_rgb},
     {0x000000_rgb, 0x000000_rgb, 0x000000_rgb, 0x000000_rgb},
     {0x000000_rgb, 0x000000_rgb, 0x000000_rgb, 0x000000_rgb}}
};

/* Output of PVRTexTool with format conversion. This is PatternRgbData[2],
    but each byte extended to uint by just repeating the byte 4 times. */
constexpr UnsignedInt HalfU = 0x7f7f7f7f;
constexpr UnsignedInt FullU = 0xffffffff;
constexpr Math::Color3<UnsignedInt> PatternRgb32UIData[4*3]{
    {    0,     0, FullU}, {    0, FullU,     0}, {HalfU, 0, HalfU}, {HalfU,     0, HalfU},
    {FullU, FullU, FullU}, {FullU,     0,     0}, {    0, 0,     0}, {    0, FullU,     0},
    {FullU,     0,     0}, {FullU, FullU, FullU}, {    0, 0,     0}, {    0, FullU,     0}
};

/* Output of PVRTexTool with format conversion. This is PatternRgbData[2],
    but each byte mapped to the range 0.0 - 1.0. */
constexpr Float HalfF = 127.0f / 255.0f;
constexpr Math::Color3<Float> PatternRgb32FData[4*3]{
    {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {HalfF, 0.0f, HalfF}, {HalfF, 0.0f, HalfF},
    {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f,  0.0f,  0.0f}, {0.0f,  1.0f,  0.0f},
    {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.0f,  0.0f,  0.0f}, {0.0f,  1.0f,  0.0f}
};

constexpr UnsignedByte PatternStencil8UIData[4*3]{
    1,  2,  3,  4,
    5,  6,  7,  8,
    9, 10, 11, 12
};

constexpr UnsignedShort PatternDepth16UnormData[4*3]{
    0xff01, 0xff02, 0xff03, 0xff04,
    0xff05, 0xff06, 0xff07, 0xff08,
    0xff09, 0xff10, 0xff11, 0xff12
};

constexpr UnsignedInt PatternDepth24UnormStencil8UIData[4*3]{
    0xffffff01, 0xffffff02, 0xffffff03, 0xffffff04,
    0xffffff05, 0xffffff06, 0xffffff07, 0xffffff08,
    0xffffff09, 0xffffff10, 0xffffff11, 0xffffff12
};

constexpr UnsignedLong HalfL = 0x7f7f7f7f7f7f7f7f;
constexpr UnsignedLong FullL = 0xffffffffffffffff;
constexpr UnsignedLong PatternDepth32FStencil8UIData[4*3]{
    0,     0,     0, HalfL,
    0, FullL, FullL, HalfL,
    0, FullL,     0, FullL
};

const char* WriterToktx = "toktx v4.0.0~6 / libktx v4.0.0~5";
const char* WriterPVRTexTool = "PVRTexLib v5.1.0";

const struct {
    const char* name;
    const Vector3i size;
    const char* message;
} TooManyLevelsData[]{
    {"1D", {1, 0, 0}, "there can be only 1 levels with base image size Vector(1) but got 2"},
    {"2D", {1, 1, 0}, "there can be only 1 levels with base image size Vector(1, 1) but got 2"},
    {"3D", {1, 1, 1}, "there can be only 1 levels with base image size Vector(1, 1, 1) but got 2"}
};

const struct {
    const char* name;
    const Vector3i sizes[2];
    const char* message;
} LevelWrongSizeData[]{
    {"1D", {{4, 0, 0}, {3, 0, 0}}, "expected size Vector(2) for level 1 but got Vector(3)"},
    {"2D", {{4, 5, 0}, {2, 1, 0}}, "expected size Vector(2, 2) for level 1 but got Vector(2, 1)"},
    {"3D", {{4, 5, 3}, {2, 2, 2}}, "expected size Vector(2, 2, 1) for level 1 but got Vector(2, 2, 2)"}
};

const struct {
    const char* name;
    const char* file;
    const CompressedPixelFormat format;
    const Math::Vector<1, Int> size;
} Convert1DCompressedData[]{
    {"BC1", "1d-compressed-bc1.ktx2", CompressedPixelFormat::Bc1RGBASrgb, {4}},
    {"ETC2", "1d-compressed-etc2.ktx2", CompressedPixelFormat::Etc2RGB8Srgb, {7}}
};

const struct {
    const char* name;
    const char* file;
    const CompressedPixelFormat format;
    const Vector2i size;
} Convert2DCompressedData[]{
    {"PVRTC", "2d-compressed-pvrtc.ktx2", CompressedPixelFormat::PvrtcRGBA4bppSrgb, {8, 8}},
    {"BC1", "2d-compressed-bc1.ktx2", CompressedPixelFormat::Bc1RGBASrgb, {8, 8}},
    {"BC3", "2d-compressed-bc3.ktx2", CompressedPixelFormat::Bc3RGBASrgb, {8, 8}},
    {"ETC2", "2d-compressed-etc2.ktx2", CompressedPixelFormat::Etc2RGB8Srgb, {9, 10}},
    {"ASTC", "2d-compressed-astc.ktx2", CompressedPixelFormat::Astc12x10RGBASrgb, {9, 10}}
};

const struct {
    const char* name;
    const char* file;
    const char* orientation;
    const char* writer;
    const PixelFormat format;
    const Containers::ArrayView<const char> data;
    bool save; /** @todo dafuq, replace all with Compare::DataToFile */
} ConvertFormatsData[]{
    {"RGB32UI", "2d-rgb32.ktx2", "rd", WriterPVRTexTool, PixelFormat::RGB32UI,
        Containers::arrayCast<const char>(PatternRgb32UIData), false},
    {"RGB32F", "2d-rgbf32.ktx2", "rd", WriterPVRTexTool, PixelFormat::RGB32F,
        Containers::arrayCast<const char>(PatternRgb32FData), false},
    /* These are saved as test files for KtxImporterTest */
    {"Stencil8UI", "2d-s8.ktx2", nullptr, nullptr, PixelFormat::Stencil8UI,
        Containers::arrayCast<const char>(PatternStencil8UIData), true},
    {"Depth16Unorm", "2d-d16.ktx2", nullptr, nullptr, PixelFormat::Depth16Unorm,
        Containers::arrayCast<const char>(PatternDepth16UnormData), true},
    {"Depth24UnormStencil8UI", "2d-d24s8.ktx2", nullptr, nullptr, PixelFormat::Depth24UnormStencil8UI,
        Containers::arrayCast<const char>(PatternDepth24UnormStencil8UIData), true},
    {"Depth32FStencil8UI", "2d-d32fs8.ktx2", nullptr, nullptr, PixelFormat::Depth32FStencil8UI,
        Containers::arrayCast<const char>(PatternDepth32FStencil8UIData), true}
};

const struct {
    const char* name;
    const CompressedPixelFormat inputFormat;
    const CompressedPixelFormat outputFormat;
} PvrtcRgbData[]{
    {"2bppUnorm", CompressedPixelFormat::PvrtcRGB2bppUnorm, CompressedPixelFormat::PvrtcRGBA2bppUnorm},
    {"2bppSrgb", CompressedPixelFormat::PvrtcRGB2bppSrgb, CompressedPixelFormat::PvrtcRGBA2bppSrgb},
    {"4bppUnorm", CompressedPixelFormat::PvrtcRGB4bppUnorm, CompressedPixelFormat::PvrtcRGBA4bppUnorm},
    {"4bppSrgb", CompressedPixelFormat::PvrtcRGB4bppSrgb, CompressedPixelFormat::PvrtcRGBA4bppSrgb},
};

const struct {
    const char* name;
    ImageConverterFlags flags;
    bool quiet;
} QuietData[]{
    {"", {}, false},
    {"quiet", ImageConverterFlag::Quiet, true}
};

const struct {
    const char* name;
    const char* value;
    ImageFlags3D imageFlags;
    const char* message;
} InvalidOrientationData[]{
    {"too short", "rd", {},
        "invalid orientation string, expected at least 3 characters but got rd"},
    {"too short for an array", "r", ImageFlag3D::Array,
        "invalid orientation string, expected at least 2 characters but got r"},
    {"invalid character", "xxx", {},
        "invalid character in orientation, expected r or l but got x"},
    {"invalid order", "rid", {},
        "invalid character in orientation, expected d or u but got i"},
};

const struct {
    const char* name;
    const char* value;
    const char* message;
} InvalidSwizzleData[]{
    {"too short", "r", "invalid swizzle length, expected 4 but got 1"},
    {"invalid characters", "rxba", "invalid characters in swizzle rxba"},
    {"invalid characters", "1012", "invalid characters in swizzle 1012"}
};

Containers::Array<char> readDataFormatDescriptor(Containers::ArrayView<const char> fileData) {
    CORRADE_INTERNAL_ASSERT(fileData.size() >= sizeof(Implementation::KtxHeader));
    const Implementation::KtxHeader& header = *reinterpret_cast<const Implementation::KtxHeader*>(fileData.data());

    const UnsignedInt offset = Utility::Endianness::littleEndian(header.dfdByteOffset);
    const UnsignedInt length = Utility::Endianness::littleEndian(header.dfdByteLength);

    return Containers::Array<char>{InPlaceInit, fileData.sliceSize(offset, length)};
}

Containers::String readKeyValueData(Containers::ArrayView<const char> fileData) {
    CORRADE_INTERNAL_ASSERT(fileData.size() >= sizeof(Implementation::KtxHeader));
    const Implementation::KtxHeader& header = *reinterpret_cast<const Implementation::KtxHeader*>(fileData.data());

    const UnsignedInt offset = Utility::Endianness::littleEndian(header.kvdByteOffset);
    const UnsignedInt length = Utility::Endianness::littleEndian(header.kvdByteLength);

    return fileData.sliceSize(offset, length);
}

KtxImageConverterTest::KtxImageConverterTest() {
    addTests({&KtxImageConverterTest::supportedFormat,
              &KtxImageConverterTest::supportedCompressedFormat,
              &KtxImageConverterTest::unsupportedFormat,
              &KtxImageConverterTest::unsupportedCompressedFormat,
              &KtxImageConverterTest::implementationSpecificFormat,
              &KtxImageConverterTest::implementationSpecificCompressedFormat,

              &KtxImageConverterTest::dataFormatDescriptor,
              &KtxImageConverterTest::dataFormatDescriptorCompressed,

              &KtxImageConverterTest::pixelStorage});

    addInstancedTests({&KtxImageConverterTest::tooManyLevels},
        Containers::arraySize(TooManyLevelsData));

    addInstancedTests({&KtxImageConverterTest::levelWrongSize},
        Containers::arraySize(LevelWrongSizeData));

    addTests({&KtxImageConverterTest::convert1D,
              &KtxImageConverterTest::convert1DMipmaps});

    addInstancedTests({&KtxImageConverterTest::convert1DCompressed},
        Containers::arraySize(Convert1DCompressedData));

    addTests({&KtxImageConverterTest::convert1DCompressedMipmaps,

              &KtxImageConverterTest::convert1DArray,

              &KtxImageConverterTest::convert2D,
              &KtxImageConverterTest::convert2DMipmaps,
              &KtxImageConverterTest::convert2DMipmapsIncomplete});

    addInstancedTests({&KtxImageConverterTest::convert2DCompressed},
        Containers::arraySize(Convert2DCompressedData));

    addTests({&KtxImageConverterTest::convert2DCompressedMipmaps,

              &KtxImageConverterTest::convert2DArray,
              &KtxImageConverterTest::convert2DArrayMipmaps,

              &KtxImageConverterTest::convertCubeMap,
              &KtxImageConverterTest::convertCubeMapMipmaps,
              &KtxImageConverterTest::convertCubeMapArray,

              &KtxImageConverterTest::convert3D,
              &KtxImageConverterTest::convert3DMipmaps,
              &KtxImageConverterTest::convert3DCompressed,
              &KtxImageConverterTest::convert3DCompressedMipmaps});

    addInstancedTests({&KtxImageConverterTest::convertFormats},
        Containers::arraySize(ConvertFormatsData));

    addInstancedTests({&KtxImageConverterTest::pvrtcRgb},
        Containers::arraySize(PvrtcRgbData));

    addTests({&KtxImageConverterTest::configurationOrientation,
              &KtxImageConverterTest::configurationOrientationLessDimensions});

    addInstancedTests({&KtxImageConverterTest::configurationOrientationEmpty},
        Containers::arraySize(QuietData));

    addInstancedTests({&KtxImageConverterTest::configurationOrientationInvalid},
        Containers::arraySize(InvalidOrientationData));

    addTests({&KtxImageConverterTest::configurationSwizzle,
              &KtxImageConverterTest::configurationSwizzleEmpty});

    addInstancedTests({&KtxImageConverterTest::configurationSwizzleInvalid},
        Containers::arraySize(InvalidSwizzleData));

    addTests({&KtxImageConverterTest::configurationGenerator,
              &KtxImageConverterTest::configurationGeneratorVersion,
              &KtxImageConverterTest::configurationGeneratorEmpty});

    addInstancedTests({&KtxImageConverterTest::configurationEmpty},
        Containers::arraySize(QuietData));

    addTests({&KtxImageConverterTest::configurationSorted,

              &KtxImageConverterTest::convertTwice});

    /* Load the plugin directly from the build tree. Otherwise it's static and
       already loaded. */
    #ifdef KTXIMAGECONVERTER_PLUGIN_FILENAME
    CORRADE_INTERNAL_ASSERT_OUTPUT(_converterManager.load(KTXIMAGECONVERTER_PLUGIN_FILENAME) & PluginManager::LoadState::Loaded);
    #endif
    /* Optional plugins that don't have to be here */
    #ifdef KTXIMPORTER_PLUGIN_FILENAME
    CORRADE_INTERNAL_ASSERT_OUTPUT(_importerManager.load(KTXIMPORTER_PLUGIN_FILENAME) & PluginManager::LoadState::Loaded);
    #endif

    /* Extract VkFormat and DFD content from merged DFD file */
    dfdData = *CORRADE_INTERNAL_ASSERT_EXPRESSION(Utility::Path::read(Utility::Path::join(KTXIMAGECONVERTER_TEST_DIR, "dfd-data.bin")));
    CORRADE_INTERNAL_ASSERT(!dfdData.isEmpty());
    CORRADE_INTERNAL_ASSERT(dfdData.size()%4 == 0);
    std::size_t offset = 0;
    while(offset < dfdData.size()) {
        /* Each entry is a VkFormat, followed directly by the DFD. The first
           uint32_t of the DFD is its size. */
        const Implementation::VkFormat format = *reinterpret_cast<Implementation::VkFormat*>(dfdData.data() + offset);
        offset += sizeof(format);
        const UnsignedInt size = *reinterpret_cast<UnsignedInt*>(dfdData.data() + offset);
        CORRADE_INTERNAL_ASSERT(size > 0);
        CORRADE_INTERNAL_ASSERT(size%4 == 0);
        dfdMap.emplace(format, dfdData.sliceSize(offset, size));
        offset += size;
    }
    CORRADE_INTERNAL_ASSERT(offset == dfdData.size());

    /* Drop version info from the generator name for predictable output.
       Remember the original value however, for the
       configurationGeneratorVersion() test case. */
    Utility::ConfigurationGroup& configuration = CORRADE_INTERNAL_ASSERT_EXPRESSION(_converterManager.metadata("KtxImageConverter"))->configuration();
    _originalGeneratorName = configuration.value<Containers::StringView>("generator");
    configuration.setValue("generator", "Magnum KtxImageConverter");
}

void KtxImageConverterTest::supportedFormat() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[32]{};

    /* All the formats in PixelFormat are supported */
    /** @todo This needs to be extended when new formats are added to
        PixelFormat. In dataFormatDescriptor() as well. Ideally Magnum itself
        should provide some kind of a "pixel format count" constant. */
    constexpr PixelFormat start = PixelFormat::R8Unorm;
    constexpr PixelFormat end = PixelFormat::Depth32FStencil8UI;

    for(UnsignedInt format = UnsignedInt(start); format <= UnsignedInt(end); ++format) {
        CORRADE_ITERATION(format);
        CORRADE_INTERNAL_ASSERT(Containers::arraySize(bytes) >= pixelFormatSize(PixelFormat(format)));
        CORRADE_VERIFY(converter->convertToData(ImageView2D{PixelFormat(format), {1, 1}, bytes}));
    }
}

void KtxImageConverterTest::supportedCompressedFormat() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[32]{};

    /** @todo This needs to be extended when new formats are added to
        CompressedPixelFormat. In dataFormatDescriptorCompressed() as well.
        Ideally Magnum itself should provide some kind of a "pixel format
        count" constant. */
    constexpr CompressedPixelFormat start = CompressedPixelFormat::Bc1RGBUnorm;
    constexpr CompressedPixelFormat end = CompressedPixelFormat::PvrtcRGBA4bppSrgb;

    for(UnsignedInt format = UnsignedInt(start); format <= UnsignedInt(end); ++format) {
        CORRADE_ITERATION(format);
        CORRADE_VERIFY(Containers::arraySize(bytes) >= compressedPixelFormatBlockDataSize(CompressedPixelFormat(format)));
        CORRADE_VERIFY(converter->convertToData(CompressedImageView2D{CompressedPixelFormat(format), {1, 1}, bytes}));
    }
}

void KtxImageConverterTest::unsupportedFormat() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    /* And implementation-specific formats have a different failure path,
       tested in implementationSpecificFormat() below */
    CORRADE_SKIP("No PixelFormat values that wouldn't be supported by KTX exist.");
}

void KtxImageConverterTest::unsupportedCompressedFormat() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    /* Compared to unsupportedFormat(), here we can abuse the fact that
       CompressedImageView so far doesn't rely on CompressedPixelFormat being
       valid to fetch block size properties for it. Once that's implemented,
       we won't be able, and then the failure should probably become an
       unreachable assert instead. */
    Containers::String out;
    Error redirectError{&out};
    CORRADE_VERIFY(!converter->convertToData(CompressedImageView2D{CompressedPixelFormat(0xffff), {1, 1}, "hello"}));
    CORRADE_COMPARE(out, "Trade::KtxImageConverter::convertToData(): unsupported format CompressedPixelFormat(0xffff)\n");
}

void KtxImageConverterTest::implementationSpecificFormat() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[1]{};

    Containers::String out;
    Error redirectError{&out};

    PixelStorage storage;
    storage.setAlignment(1);
    CORRADE_VERIFY(!converter->convertToData(ImageView2D{storage, 0, 0, 1, {1, 1}, bytes}));
    CORRADE_COMPARE(out,
        "Trade::KtxImageConverter::convertToData(): implementation-specific formats are not supported\n");
}

void KtxImageConverterTest::implementationSpecificCompressedFormat() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[1]{};

    Containers::String out;
    Error redirectError{&out};

    CompressedPixelStorage storage;
    CORRADE_VERIFY(!converter->convertToData(CompressedImageView2D{storage, 0, {1, 1}, bytes}));
    CORRADE_COMPARE(out,
        "Trade::KtxImageConverter::convertToData(): implementation-specific formats are not supported\n");
}

void KtxImageConverterTest::dataFormatDescriptor() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[32]{};

    constexpr PixelFormat start = PixelFormat::R8Unorm;
    constexpr PixelFormat end = PixelFormat::Depth32FStencil8UI;

    for(UnsignedInt format = UnsignedInt(start); format <= UnsignedInt(end); ++format) {
        CORRADE_ITERATION(format);
        CORRADE_VERIFY(Containers::arraySize(bytes) >= pixelFormatSize(PixelFormat(format)));
        Containers::Optional<Containers::Array<char>> output = converter->convertToData(ImageView2D{PixelFormat(format), {1, 1}, bytes});
        CORRADE_VERIFY(output);

        const Implementation::KtxHeader& header = *reinterpret_cast<const Implementation::KtxHeader*>(output->data());
        const Implementation::VkFormat vkFormat = Utility::Endianness::littleEndian(header.vkFormat);

        Containers::Array<char> dfd = readDataFormatDescriptor(*output);
        CORRADE_COMPARE(dfdMap.count(vkFormat), 1);
        CORRADE_COMPARE_AS(dfd, dfdMap[vkFormat], TestSuite::Compare::Container);
    }
}

void KtxImageConverterTest::dataFormatDescriptorCompressed() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[32]{};

    constexpr CompressedPixelFormat start = CompressedPixelFormat::Bc1RGBUnorm;
    constexpr CompressedPixelFormat end = CompressedPixelFormat::PvrtcRGBA4bppSrgb;

    for(UnsignedInt format = UnsignedInt(start); format <= UnsignedInt(end); ++format) {
        CORRADE_ITERATION(format);
        CORRADE_VERIFY(Containers::arraySize(bytes) >= compressedPixelFormatBlockDataSize(CompressedPixelFormat(format)));
        Containers::Optional<Containers::Array<char>> output = converter->convertToData(CompressedImageView2D{CompressedPixelFormat(format), {1, 1}, bytes});
        CORRADE_VERIFY(output);

        const Implementation::KtxHeader& header = *reinterpret_cast<const Implementation::KtxHeader*>(output->data());
        const Implementation::VkFormat vkFormat = Utility::Endianness::littleEndian(header.vkFormat);

        Containers::Array<char> dfd = readDataFormatDescriptor(*output);
        CORRADE_COMPARE(dfdMap.count(vkFormat), 1);
        CORRADE_COMPARE_AS(dfd, dfdMap[vkFormat], TestSuite::Compare::Container);
    }
}

void KtxImageConverterTest::pixelStorage() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    constexpr UnsignedByte bytes[4*3]{
        0, 1, 2, 3,
        4, 5, 6, 7,
        8, 9, 10, 11
    };

    PixelStorage storage;
    storage.setAlignment(4);
    storage.setSkip({1, 1, 0});

    const ImageView2D inputImage{storage, PixelFormat::R8UI, {2, 2}, Containers::arrayView(bytes)};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    if(_importerManager.loadState("KtxImporter") == PluginManager::LoadState::NotFound)
        CORRADE_SKIP("KtxImporter plugin not found, cannot test");

    Containers::Pointer<AbstractImporter> importer = _importerManager.instantiate("KtxImporter");
    CORRADE_VERIFY(importer->openData(*output));

    Containers::Optional<Trade::ImageData2D> image = importer->image2D(0);
    CORRADE_VERIFY(image);
    CORRADE_COMPARE_AS(image->data(), Containers::arrayView<char>({5, 6, 9, 10}), TestSuite::Compare::Container);
}

void KtxImageConverterTest::tooManyLevels() {
    auto&& data = TooManyLevelsData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[4]{};
    CORRADE_INTERNAL_ASSERT(Math::max(Vector3ui{data.size}, 1u).product()*4u <= Containers::arraySize(bytes));

    const UnsignedInt dimensions = Math::min(Vector3ui{data.size}, 1u).sum();

    Containers::String out;
    Error redirectError{&out};
    if(dimensions == 1) {
        CORRADE_VERIFY(!converter->convertToData({
            ImageView1D{PixelFormat::RGBA8Unorm, data.size.x(), bytes},
            ImageView1D{PixelFormat::RGBA8Unorm, data.size.x(), bytes}
        }));
    } else if(dimensions == 2) {
        CORRADE_VERIFY(!converter->convertToData({
            ImageView2D{PixelFormat::RGBA8Unorm, data.size.xy(), bytes},
            ImageView2D{PixelFormat::RGBA8Unorm, data.size.xy(), bytes}
        }));
    } else if(dimensions == 3) {
        CORRADE_VERIFY(!converter->convertToData({
            ImageView3D{PixelFormat::RGBA8Unorm, data.size, bytes},
            ImageView3D{PixelFormat::RGBA8Unorm, data.size, bytes}
        }));
    }

    CORRADE_COMPARE(out, Utility::format("Trade::KtxImageConverter::convertToData(): {}\n", data.message));
}

void KtxImageConverterTest::levelWrongSize() {
    auto&& data = LevelWrongSizeData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[256]{};
    CORRADE_INTERNAL_ASSERT(Math::max(Vector3ui{data.sizes[0]}, 1u).product()*4u <= Containers::arraySize(bytes));

    const UnsignedInt dimensions = Math::min(Vector3ui{data.sizes[0]}, 1u).sum();

    Containers::String out;
    Error redirectError{&out};
    if(dimensions == 1) {
        CORRADE_VERIFY(!converter->convertToData({
            ImageView1D{PixelFormat::RGBA8Unorm, data.sizes[0].x(), bytes},
            ImageView1D{PixelFormat::RGBA8Unorm, data.sizes[1].x(), bytes}
        }));
    } else if(dimensions == 2) {
        CORRADE_VERIFY(!converter->convertToData({
            ImageView2D{PixelFormat::RGBA8Unorm, data.sizes[0].xy(), bytes},
            ImageView2D{PixelFormat::RGBA8Unorm, data.sizes[1].xy(), bytes}
        }));
    } else if(dimensions == 3) {
        CORRADE_VERIFY(!converter->convertToData({
            ImageView3D{PixelFormat::RGBA8Unorm, data.sizes[0], bytes},
            ImageView3D{PixelFormat::RGBA8Unorm, data.sizes[1], bytes}
        }));
    }

    CORRADE_COMPARE(out, Utility::format("Trade::KtxImageConverter::convertToData(): {}\n", data.message));
}

void KtxImageConverterTest::convert1D() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    /* toktx 4.0 has a bug and doesn't write KTXorientation for 1D images, 4.1
       RC does. To avoid unnecessary warning when testing opening 1D files in
       KtxImporter, the file is faked and produced by KtxImageConverterTest
       instead of toktx */
    /** @todo regenerate the test files when toktx 4.1 is stable */
    converter->configuration().setValue("orientation", "r");
    converter->configuration().setValue("generator", WriterToktx);

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView1D inputImage{storage, PixelFormat::RGB8Srgb, {4}, PatternRgb1DData[0]};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);
    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "1d.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convert1DMipmaps() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    /* toktx 4.0 has a bug and doesn't write KTXorientation for 1D images, 4.1
       RC does. To avoid unnecessary warning when testing opening 1D files in
       KtxImporter, the file is faked and produced by KtxImageConverterTest
       instead of toktx */
    /** @todo regenerate the test files when toktx 4.1 is stable */
    converter->configuration().setValue("orientation", "r");
    converter->configuration().setValue("generator", WriterToktx);

    constexpr Math::Vector<1, Int> size{4};
    const Color3ub mip0[4]{0xff0000_rgb, 0xffffff_rgb, 0x000000_rgb, 0x007f7f_rgb};
    const Color3ub mip1[2]{0xffffff_rgb, 0x007f7f_rgb};
    const Color3ub mip2[1]{0x000000_rgb};

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView1D inputImages[3]{
        ImageView1D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 0, 1), mip0},
        ImageView1D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 1, 1), mip1},
        ImageView1D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 2, 1), mip2}
    };

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImages);
    CORRADE_VERIFY(output);
    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "1d-mipmaps.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convert1DCompressed() {
    auto&& data = Convert1DCompressedData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "r");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    Containers::Optional<Containers::Array<char>> blockData = Utility::Path::read(
        Utility::Path::join(KTXIMPORTER_TEST_DIR, Utility::Path::splitExtension(data.file).first() + ".bin"));
    CORRADE_VERIFY(blockData);
    const CompressedImageView1D inputImage{data.format, data.size, *blockData};

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, data.file));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert1DCompressedMipmaps() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "r");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    constexpr Math::Vector<1, Int> size{7};
    Containers::Optional<Containers::Array<char>> mip0 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "1d-compressed-mipmaps-mip0.bin"));
    Containers::Optional<Containers::Array<char>> mip1 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "1d-compressed-mipmaps-mip1.bin"));
    Containers::Optional<Containers::Array<char>> mip2 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "1d-compressed-mipmaps-mip2.bin"));
    CORRADE_VERIFY(mip0);
    CORRADE_VERIFY(mip1);
    CORRADE_VERIFY(mip2);

    const CompressedImageView1D inputImages[3]{
        CompressedImageView1D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 0, 1), *mip0},
        CompressedImageView1D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 1, 1), *mip1},
        CompressedImageView1D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 2, 1), *mip2}
    };

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImages);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "1d-compressed-mipmaps.ktx2"));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert1DArray() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView2D inputImage{storage, PixelFormat::RGB8Srgb, {4, 3}, PatternRgb1DData, ImageFlag2D::Array};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "1d-layers.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convert2D() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    CORRADE_COMPARE(converter->extension(), "ktx2");
    CORRADE_COMPARE(converter->mimeType(), "image/ktx2");

    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterToktx);

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView2D inputImage{storage, PixelFormat::RGB8Srgb, {4, 3}, PatternRgbData[Containers::arraySize(PatternRgbData) - 1]};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-rgb.ktx2"));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert2DMipmaps() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterToktx);

    constexpr Vector2i size{4, 3};
    const auto mip0 = Containers::arrayCast<const Color3ub>(Containers::arrayView(
        PatternRgbData[Containers::arraySize(PatternRgbData) - 1]));
    const Color3ub mip1[2]{0xffffff_rgb, 0x007f7f_rgb};
    const Color3ub mip2[1]{0x000000_rgb};

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView2D inputImages[3]{
        ImageView2D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 0, 1), mip0},
        ImageView2D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 1, 1), mip1},
        ImageView2D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 2, 1), mip2}
    };

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImages);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-mipmaps.ktx2"));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert2DMipmapsIncomplete() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterToktx);

    constexpr Vector2i size{4, 3};
    const auto mip0 = Containers::arrayCast<const Color3ub>(Containers::arrayView(
        PatternRgbData[Containers::arraySize(PatternRgbData) - 1]));
    const Color3ub mip1[2]{0xffffff_rgb, 0x007f7f_rgb};

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView2D inputImages[2]{
        ImageView2D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 0, 1), mip0},
        ImageView2D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 1, 1), mip1}
    };

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImages);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-mipmaps-incomplete.ktx2"));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert2DCompressed() {
    auto&& data = Convert2DCompressedData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    Containers::Optional<Containers::Array<char>> blockData = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, Utility::Path::splitExtension(data.file).first() + ".bin"));
    CORRADE_VERIFY(blockData);
    const CompressedImageView2D inputImage{data.format, data.size, *blockData};

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, data.file));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert2DCompressedMipmaps() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    constexpr Vector2i size{9, 10};
    Containers::Optional<Containers::Array<char>> mip0 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-compressed-mipmaps-mip0.bin"));
    Containers::Optional<Containers::Array<char>> mip1 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-compressed-mipmaps-mip1.bin"));
    Containers::Optional<Containers::Array<char>> mip2 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-compressed-mipmaps-mip2.bin"));
    Containers::Optional<Containers::Array<char>> mip3 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-compressed-mipmaps-mip3.bin"));
    CORRADE_VERIFY(mip0);
    CORRADE_VERIFY(mip1);
    CORRADE_VERIFY(mip2);
    CORRADE_VERIFY(mip3);

    const CompressedImageView2D inputImages[4]{
        CompressedImageView2D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 0, 1), *mip0},
        CompressedImageView2D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 1, 1), *mip1},
        CompressedImageView2D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 2, 1), *mip2},
        CompressedImageView2D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 3, 1), *mip3}
    };

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImages);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-compressed-mipmaps.ktx2"));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert2DArray() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView3D inputImage{storage, PixelFormat::RGB8Srgb, {4, 3, 3}, PatternRgbData2DArray, ImageFlag3D::Array};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-layers.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convert2DArrayMipmaps() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    const ImageView3D inputImage0{PixelStorage{}.setAlignment(1), PixelFormat::RGB8Srgb, {4, 3, 3}, PatternRgbData2DArray, ImageFlag3D::Array};

    /* Height is 1, so this isn't Y-flipped compared to KtxImporterTest */
    const Color3ub mip1[2*1*3]{
        0x0000ff_rgb, 0x7f007f_rgb,
        0x0000ff_rgb, 0x7f007f_rgb,
        0x000000_rgb, 0x000000_rgb
    };
    const ImageView3D inputImage1{PixelStorage{}.setAlignment(1), PixelFormat::RGB8Srgb, {2, 1, 3}, mip1, ImageFlag3D::Array};

    const Color3ub mip2[1*1*3]{
        0x0000ff_rgb,
        0x0000ff_rgb,
        0x000000_rgb
    };
    const ImageView3D inputImage2{PixelStorage{}.setAlignment(1), PixelFormat::RGB8Srgb, {1, 1, 3}, mip2, ImageFlag3D::Array};

    Containers::Optional<Containers::Array<char>> output = converter->convertToData({inputImage0, inputImage1, inputImage2});
    CORRADE_VERIFY(output);

    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "2d-mipmaps-and-layers.ktx2"),
        TestSuite::Compare::StringToFile);
}

/* Same as FacesRgbData in KtxImporterTest, but origin flipped from bottom-left
   to top-left. */
const Color3ub FacesRgbData[2][6][2][2]{
    /* cube+x.png, Y-flipped */
    {{{0x0d0d0d_rgb, 0x0d0d0d_rgb},
      {0xffffff_rgb, 0x0d0d0d_rgb}},
    /* cube-x.png, Y-flipped */
     {{0x222222_rgb, 0x222222_rgb},
      {0xffffff_rgb, 0x222222_rgb}},
    /* cube+y.png, Y-flipped */
     {{0x323232_rgb, 0x323232_rgb},
      {0xffffff_rgb, 0x323232_rgb}},
    /* cube-y.png, Y-flipped */
     {{0x404040_rgb, 0x404040_rgb},
      {0xffffff_rgb, 0x404040_rgb}},
    /* cube+z.png, Y-flipped */
     {{0x4f4f4f_rgb, 0x4f4f4f_rgb},
      {0xffffff_rgb, 0x4f4f4f_rgb}},
    /* cube-z.png, Y-flipped */
     {{0x606060_rgb, 0x606060_rgb},
      {0xffffff_rgb, 0x606060_rgb}}},

    /* cube+z.png, Y-flipped */
    {{{0x4f4f4f_rgb, 0x4f4f4f_rgb},
      {0xffffff_rgb, 0x4f4f4f_rgb}},
    /* cube-z.png */
     {{0x606060_rgb, 0x606060_rgb},
      {0xffffff_rgb, 0x606060_rgb}},
    /* cube+x.png */
     {{0x0d0d0d_rgb, 0x0d0d0d_rgb},
      {0xffffff_rgb, 0x0d0d0d_rgb}},
    /* cube-x.png */
     {{0x222222_rgb, 0x222222_rgb},
      {0xffffff_rgb, 0x222222_rgb}},
    /* cube+y.png */
     {{0x323232_rgb, 0x323232_rgb},
      {0xffffff_rgb, 0x323232_rgb}},
    /* cube-y.png */
     {{0x404040_rgb, 0x404040_rgb},
      {0xffffff_rgb, 0x404040_rgb}}}
};

void KtxImageConverterTest::convertCubeMap() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    const ImageView3D inputImage{PixelStorage{}.setAlignment(1), PixelFormat::RGB8Srgb, {2, 2, 6}, FacesRgbData, ImageFlag3D::CubeMap};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "cubemap.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convertCubeMapMipmaps() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    const ImageView3D inputImage0{PixelStorage{}.setAlignment(1), PixelFormat::RGB8Srgb, {2, 2, 6}, FacesRgbData, ImageFlag3D::CubeMap};

    /* Because FacesRgbData is Y-flipped compared to KtxImporterTest, this
       also takes the first row instead of the last row */
    const Color3ub mip1[1*1*6]{
        FacesRgbData[0][0][0][0],
        FacesRgbData[0][1][0][0],
        FacesRgbData[0][2][0][0],
        FacesRgbData[0][3][0][0],
        FacesRgbData[0][4][0][0],
        FacesRgbData[0][5][0][0]
    };
    const ImageView3D inputImage1{PixelStorage{}.setAlignment(1), PixelFormat::RGB8Srgb, {1, 1, 6}, mip1, ImageFlag3D::CubeMap};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData({inputImage0, inputImage1});
    CORRADE_VERIFY(output);

    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "cubemap-mipmaps.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convertCubeMapArray() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rd");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    const ImageView3D inputImage{PixelStorage{}.setAlignment(1), PixelFormat::RGB8Srgb, {2, 2, 12}, FacesRgbData, ImageFlag3D::CubeMap|ImageFlag3D::Array};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "cubemap-layers.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convert3D() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rdi");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView3D inputImage{storage, PixelFormat::RGB8Srgb, {4, 3, 3}, PatternRgbData};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d.ktx2"));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert3DMipmaps() {
    /* Neither toktx nor PVRTexTool can create mipmapped 3D textures. We use
       the converter to create our own test file for the importer and the
       converter ground truth. At the very least it catches unexpected changes.
       Save it by running the test with:
       --save-diagnostic [path/to/KtxImporter/Test] */

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rdi");

    const Vector3i size{4, 3, 3};
    const auto mip0 = Containers::arrayCast<const Color3ub>(Containers::arrayView(PatternRgbData));
    const Color3ub mip1[2]{0xffffff_rgb, 0x007f7f_rgb};
    const Color3ub mip2[1]{0x000000_rgb};

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView3D inputImages[3]{
        ImageView3D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 0, 1), mip0},
        ImageView3D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 1, 1), mip1},
        ImageView3D{storage, PixelFormat::RGB8Srgb, Math::max(size >> 2, 1), mip2}
    };

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImages);
    CORRADE_VERIFY(output);

    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-mipmaps.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convert3DCompressed() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rdi");
    converter->configuration().setValue("generator", WriterPVRTexTool);

    Containers::Optional<Containers::Array<char>> blockData = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-compressed-etc2rgb8.bin"));
    CORRADE_VERIFY(blockData);
    const CompressedImageView3D inputImage{CompressedPixelFormat::Etc2RGB8Srgb, {9, 10, 3}, *blockData};

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-compressed-etc2rgb8.ktx2"));
    CORRADE_VERIFY(expected);
    CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
}

void KtxImageConverterTest::convert3DCompressedMipmaps() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->configuration().setValue("orientation", "rdi");

    /* Same as convert3DMipmaps, we generate this file here because none of the
       tools can do it. The other compressed .bin data is extracted from files
       created by toktx/PVRTexTool. In this case we handishly created data from
       existing 2D ETC2 data. Oh well, better than nothing until there's a
       better way to generate these images. */

    constexpr Vector3i size{9, 10, 5};
    Containers::Optional<Containers::Array<char>> mip0 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-compressed-mipmaps-mip0.bin"));
    Containers::Optional<Containers::Array<char>> mip1 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-compressed-mipmaps-mip1.bin"));
    Containers::Optional<Containers::Array<char>> mip2 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-compressed-mipmaps-mip2.bin"));
    Containers::Optional<Containers::Array<char>> mip3 = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-compressed-mipmaps-mip3.bin"));
    CORRADE_VERIFY(mip0);
    CORRADE_VERIFY(mip1);
    CORRADE_VERIFY(mip2);
    CORRADE_VERIFY(mip3);

    const CompressedImageView3D inputImages[4]{
        CompressedImageView3D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 0, 1), *mip0},
        CompressedImageView3D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 1, 1), *mip1},
        CompressedImageView3D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 2, 1), *mip2},
        CompressedImageView3D{CompressedPixelFormat::Etc2RGB8Srgb, Math::max(size >> 3, 1), *mip3}
    };

    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImages);
    CORRADE_VERIFY(output);

    /** @todo Compare::DataToFile */
    Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-compressed-mipmaps.ktx2"));
    CORRADE_COMPARE_AS(Containers::StringView{*output},
        Utility::Path::join(KTXIMPORTER_TEST_DIR, "3d-compressed-mipmaps.ktx2"),
        TestSuite::Compare::StringToFile);
}

void KtxImageConverterTest::convertFormats() {
    auto&& data = ConvertFormatsData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    if(data.orientation)
        converter->configuration().setValue("orientation", data.orientation);
    if(data.writer)
        converter->configuration().setValue("generator", data.writer);

    PixelStorage storage;
    storage.setAlignment(1);
    const ImageView2D inputImage{storage, data.format, {4, 3}, data.data};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    if(data.save) {
        CORRADE_COMPARE_AS(Containers::StringView{*output},
            Utility::Path::join(KTXIMPORTER_TEST_DIR, data.file),
            TestSuite::Compare::StringToFile);
    } else {
        /** @todo Compare::DataToFile */
        Containers::Optional<Containers::Array<char>> expected = Utility::Path::read(Utility::Path::join(KTXIMPORTER_TEST_DIR, data.file));
        CORRADE_COMPARE_AS(*output, *expected, TestSuite::Compare::Container);
    }
}

void KtxImageConverterTest::pvrtcRgb() {
    auto&& data = PvrtcRgbData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[16]{};
    const UnsignedInt dataSize = compressedPixelFormatBlockDataSize(data.inputFormat);
    const Vector2i imageSize = {2, 2};
    CORRADE_INTERNAL_ASSERT(Containers::arraySize(bytes) >= dataSize);
    CORRADE_INTERNAL_ASSERT((Vector3i{imageSize, 1}) <= compressedPixelFormatBlockSize(data.inputFormat));

    const CompressedImageView2D inputImage{data.inputFormat, imageSize, Containers::arrayView(bytes).prefix(dataSize)};
    Containers::Optional<Containers::Array<char>> output = converter->convertToData(inputImage);
    CORRADE_VERIFY(output);

    if(_importerManager.loadState("KtxImporter") == PluginManager::LoadState::NotFound)
        CORRADE_SKIP("KtxImporter plugin not found, cannot test");

    Containers::Pointer<AbstractImporter> importer = _importerManager.instantiate("KtxImporter");
    CORRADE_VERIFY(importer->openData(*output));

    Containers::Optional<Trade::ImageData2D> image = importer->image2D(0);
    CORRADE_VERIFY(image);
    CORRADE_VERIFY(image->isCompressed());
    CORRADE_COMPARE(image->compressedFormat(), data.outputFormat);
    CORRADE_COMPARE_AS(image->data(), inputImage.data(), TestSuite::Compare::Container);
}

void KtxImageConverterTest::configurationOrientation() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    /* Default value */
    CORRADE_COMPARE(converter->configuration().value("orientation"), "ruo");
    CORRADE_VERIFY(converter->configuration().setValue("orientation", "ldo"));

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data = converter->convertToData(ImageView3D{PixelFormat::RGBA8Unorm, {1, 1, 1}, bytes});
    CORRADE_VERIFY(data);

    Containers::String keyValueData = readKeyValueData(*data);
    CORRADE_VERIFY(keyValueData.contains("KTXorientation\0ldo\0"_s));
}

void KtxImageConverterTest::configurationOrientationLessDimensions() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    /* Orientation string is shortened to the number of dimensions, extra characters are ignored */
    CORRADE_VERIFY(converter->configuration().setValue("orientation", "rdxxx"));

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data);

    Containers::String keyValueData = readKeyValueData(*data);
    CORRADE_VERIFY(keyValueData.contains("KTXorientation\0rd\0"_s));
}

void KtxImageConverterTest::configurationOrientationEmpty() {
    auto&& data = QuietData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->addFlags(data.flags);
    CORRADE_VERIFY(converter->configuration().setValue("orientation", ""));

    const UnsignedByte bytes[4]{};

    Containers::String out;
    Containers::Optional<Containers::Array<char>> imageData;
    {
        Warning redirectWarning{&out};
        imageData = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    }
    CORRADE_VERIFY(imageData);
    if(data.quiet)
        CORRADE_COMPARE(out, "");
    else
        CORRADE_COMPARE(out, "Trade::KtxImageConverter::convertToData(): empty orientation string, assuming right, down\n");

    /* Empty orientation isn't written to key/value data at all */
    Containers::String keyValueData = readKeyValueData(*imageData);
    CORRADE_VERIFY(!keyValueData.contains("KTXorientation"_s));
}

void KtxImageConverterTest::configurationOrientationInvalid() {
    auto&& data = InvalidOrientationData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    CORRADE_VERIFY(converter->configuration().setValue("orientation", data.value));

    const UnsignedByte bytes[4]{};

    Containers::String out;
    Error redirectError{&out};
    CORRADE_VERIFY(!converter->convertToData(ImageView3D{PixelFormat::RGBA8Unorm, {1, 1, 1}, bytes, data.imageFlags}));
    CORRADE_COMPARE(out, Utility::format("Trade::KtxImageConverter::convertToData(): {}\n", data.message));
}

void KtxImageConverterTest::configurationSwizzle() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    /* Default value */
    CORRADE_COMPARE(converter->configuration().value("swizzle"), "");
    CORRADE_VERIFY(converter->configuration().setValue("swizzle", "rgba"));

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data);

    Containers::String keyValueData = readKeyValueData(*data);
    CORRADE_VERIFY(keyValueData.contains("KTXswizzle\0rgba\0"_s));
}

void KtxImageConverterTest::configurationSwizzleEmpty() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    /* Swizzle is empty by default, tested in configurationSwizzle() */

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data);

    /* Empty swizzle isn't written to key/value data at all */
    Containers::String keyValueData = readKeyValueData(*data);
    CORRADE_VERIFY(!keyValueData.contains("KTXswizzle"_s));
}

void KtxImageConverterTest::configurationSwizzleInvalid() {
    auto&& data = InvalidSwizzleData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    CORRADE_VERIFY(converter->configuration().setValue("swizzle", data.value));

    Containers::String out;
    Error redirectError{&out};

    const UnsignedByte bytes[4]{};
    CORRADE_VERIFY(!converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes}));
    CORRADE_COMPARE(out, Utility::format("Trade::KtxImageConverter::convertToData(): {}\n", data.message));
}

void KtxImageConverterTest::configurationGenerator() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    CORRADE_VERIFY(converter->configuration().setValue("generator", "KtxImageConverterTest&$%1234@\x02\n\r\t\x15!"));

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data);

    /* Writer doesn't have to be null-terminated, don't test for \0 */
    Containers::String keyValueData = readKeyValueData(*data);
    CORRADE_VERIFY(keyValueData.contains("KTXwriter\0KtxImageConverterTest&$%1234@\x02\n\r\t\x15!"_s));
}

void KtxImageConverterTest::configurationGeneratorVersion() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    /* Put back the original value from config, which got patched for
       predictable output */
    converter->configuration().setValue("generator", _originalGeneratorName);

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data);

    /* The formatting is tested thoroughly in VersionTest */
    Containers::String keyValueData = readKeyValueData(*data);
    CORRADE_COMPARE_AS(keyValueData,
        "KTXwriter\0Magnum KtxImageConverter v",
        TestSuite::Compare::StringContains);

    /** @todo print the generator string once it's possible to safely extract
        just the value without any other random bytes */
}

void KtxImageConverterTest::configurationGeneratorEmpty() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    CORRADE_VERIFY(converter->configuration().setValue("generator", ""));

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data);

    /* Empty writer name isn't written to key/value data at all */
    Containers::String keyValueData = readKeyValueData(*data);
    CORRADE_VERIFY(!keyValueData.contains("KTXwriter"_s));
}

void KtxImageConverterTest::configurationEmpty() {
    auto&& data = QuietData[testCaseInstanceId()];
    setTestCaseDescription(data.name);

    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    converter->addFlags(data.flags);
    CORRADE_VERIFY(converter->configuration().removeValue("generator"));
    CORRADE_VERIFY(converter->configuration().removeValue("swizzle"));
    CORRADE_VERIFY(converter->configuration().removeValue("orientation"));

    const UnsignedByte bytes[4]{};

    Containers::String out;
    Containers::Optional<Containers::Array<char>> imageData;
    {
        Warning redirectWarning{&out};
        imageData = converter->convertToData(ImageView3D{PixelFormat::RGBA8Unorm, {1, 1, 1}, bytes});
    }
    CORRADE_VERIFY(imageData);
    if(data.quiet)
        CORRADE_COMPARE(out, "");
    else
        CORRADE_COMPARE(out, "Trade::KtxImageConverter::convertToData(): empty orientation string, assuming right, down, forward\n");

    /* Key/value data should not be written if it only contains empty values */

    const Implementation::KtxHeader& header = *reinterpret_cast<const Implementation::KtxHeader*>(imageData->data());
    CORRADE_COMPARE(header.kvdByteOffset, 0);
    CORRADE_COMPARE(header.kvdByteLength, 0);
}

void KtxImageConverterTest::configurationSorted() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");
    CORRADE_VERIFY(converter->configuration().setValue("generator", "x"));
    CORRADE_VERIFY(converter->configuration().setValue("swizzle", "barg"));
    CORRADE_VERIFY(converter->configuration().setValue("orientation", "rd"));

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data);

    Containers::String keyValueData = readKeyValueData(*data);
    Containers::StringView writerOffset = keyValueData.find("KTXwriter"_s);
    Containers::StringView swizzleOffset = keyValueData.find("KTXswizzle"_s);
    Containers::StringView orientationOffset = keyValueData.find("KTXorientation"_s);

    CORRADE_VERIFY(!writerOffset.isEmpty());
    CORRADE_VERIFY(!swizzleOffset.isEmpty());
    CORRADE_VERIFY(!orientationOffset.isEmpty());

    /* Entries are sorted alphabetically */
    CORRADE_VERIFY(orientationOffset.begin() < swizzleOffset.begin());
    CORRADE_VERIFY(swizzleOffset.begin() < writerOffset.begin());
}

void KtxImageConverterTest::convertTwice() {
    Containers::Pointer<AbstractImageConverter> converter = _converterManager.instantiate("KtxImageConverter");

    const UnsignedByte bytes[4]{};
    Containers::Optional<Containers::Array<char>> data1 = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data1);
    Containers::Optional<Containers::Array<char>> data2 = converter->convertToData(ImageView2D{PixelFormat::RGBA8Unorm, {1, 1}, bytes});
    CORRADE_VERIFY(data2);

    /* Shouldn't crash, output should be identical */
    CORRADE_COMPARE_AS(*data1, *data2, TestSuite::Compare::Container);
}

}}}}

CORRADE_TEST_MAIN(Magnum::Trade::Test::KtxImageConverterTest)
